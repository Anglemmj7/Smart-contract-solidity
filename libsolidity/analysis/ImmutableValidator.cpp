/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0

#include <libsolidity/analysis/ImmutableValidator.h>

#include <libsolutil/CommonData.h>

#include <range/v3/view/reverse.hpp>

using namespace solidity::frontend;
using namespace solidity::langutil;

void ImmutableValidator::analyze()
{
	m_inCreationContext = true;

	auto linearizedContracts = m_mostDerivedContract.annotation().linearizedBaseContracts | ranges::views::reverse;

	for (ContractDefinition const* contract: linearizedContracts)
		for (VariableDeclaration const* stateVar: contract->stateVariables())
			if (stateVar->value())
				stateVar->value()->accept(*this);

	for (ContractDefinition const* contract: linearizedContracts)
		for (std::shared_ptr<InheritanceSpecifier> const& inheritSpec: contract->baseContracts())
			if (auto args = inheritSpec->arguments())
				ASTNode::listAccept(*args, *this);

	for (ContractDefinition const* contract: linearizedContracts)
	{
		for (VariableDeclaration const* stateVar: contract->stateVariables())
			if (stateVar->value())
				m_initializedStateVariables.emplace(stateVar);

		if (contract->constructor())
			visitCallableIfNew(*contract->constructor());
	}

	m_inCreationContext = false;

	for (ContractDefinition const* contract: linearizedContracts)
	{
		for (auto funcDef: contract->definedFunctions())
			visitCallableIfNew(*funcDef);

		for (auto modDef: contract->functionModifiers())
			visitCallableIfNew(*modDef);
	}

	checkAllVariablesInitialized(m_mostDerivedContract.location());
}

bool ImmutableValidator::visit(Assignment const& _assignment)
{
	// Need to visit values first (rhs) as they might access other immutables.
	_assignment.rightHandSide().accept(*this);
	_assignment.leftHandSide().accept(*this);
	return false;
}


bool ImmutableValidator::visit(FunctionDefinition const& _functionDefinition)
{
	return analyseCallable(_functionDefinition);
}

bool ImmutableValidator::visit(ModifierDefinition const& _modifierDefinition)
{
	return analyseCallable(_modifierDefinition);
}

bool ImmutableValidator::visit(MemberAccess const& _memberAccess)
{
	_memberAccess.expression().accept(*this);

	if (auto contractType = dynamic_cast<ContractType const*>(_memberAccess.expression().annotation().type))
		if (!contractType->isSuper())
			// external access, no analysis needed.
			return false;

	if (auto varDecl = dynamic_cast<VariableDeclaration const*>(_memberAccess.annotation().referencedDeclaration))
		analyseVariableReference(*varDecl, _memberAccess);
	else if (auto funcType = dynamic_cast<FunctionType const*>(_memberAccess.annotation().type))
		if (funcType->kind() == FunctionType::Kind::Internal && funcType->hasDeclaration())
			visitCallableIfNew(funcType->declaration());

	return false;
}

bool ImmutableValidator::visit(IfStatement const& _ifStatement)
{
	bool prevInBranch = m_inBranch;

	_ifStatement.condition().accept(*this);

	m_inBranch = true;
	_ifStatement.trueStatement().accept(*this);

	if (auto falseStatement = _ifStatement.falseStatement())
		falseStatement->accept(*this);

	m_inBranch = prevInBranch;

	return false;
}

bool ImmutableValidator::visit(WhileStatement const& _whileStatement)
{
	bool prevInLoop = m_inLoop;
	m_inLoop = true;

	_whileStatement.condition().accept(*this);
	_whileStatement.body().accept(*this);

	m_inLoop = prevInLoop;

	return false;
}

bool ImmutableValidator::visit(TryStatement const& _tryStatement)
{
	ScopedSaveAndRestore constructorGuard{m_inTryStatement, true};

	_tryStatement.externalCall().accept(*this);

	for (auto&& clause: _tryStatement.clauses())
		if (clause)
			clause->accept(*this);

	return false;
}

void ImmutableValidator::endVisit(IdentifierPath const& _identifierPath)
{
	if (auto const callableDef = dynamic_cast<CallableDeclaration const*>(_identifierPath.annotation().referencedDeclaration))
		visitCallableIfNew(
			*_identifierPath.annotation().requiredLookup == VirtualLookup::Virtual ?
			callableDef->resolveVirtual(m_mostDerivedContract) :
			*callableDef
		);

	solAssert(!dynamic_cast<VariableDeclaration const*>(_identifierPath.annotation().referencedDeclaration), "");
}

void ImmutableValidator::endVisit(Identifier const& _identifier)
{
	if (auto const callableDef = dynamic_cast<CallableDeclaration const*>(_identifier.annotation().referencedDeclaration))
		visitCallableIfNew(*_identifier.annotation().requiredLookup == VirtualLookup::Virtual ? callableDef->resolveVirtual(m_mostDerivedContract) : *callableDef);
	if (auto const varDecl = dynamic_cast<VariableDeclaration const*>(_identifier.annotation().referencedDeclaration))
		analyseVariableReference(*varDecl, _identifier);
}

void ImmutableValidator::endVisit(Return const& _return)
{
	if (m_currentConstructor != nullptr)
		checkAllVariablesInitialized(_return.location());
}

bool ImmutableValidator::analyseCallable(CallableDeclaration const& _callableDeclaration)
{
	ScopedSaveAndRestore constructorGuard{m_currentConstructor, {}};
	ScopedSaveAndRestore constructorContractGuard{m_currentConstructorContract, {}};

	if (FunctionDefinition const* funcDef = dynamic_cast<decltype(funcDef)>(&_callableDeclaration))
	{
		ASTNode::listAccept(funcDef->modifiers(), *this);

		if (funcDef->isConstructor())
		{
			m_currentConstructorContract = funcDef->annotation().contract;
			m_currentConstructor = funcDef;
		}

		if (funcDef->isImplemented())
			funcDef->body().accept(*this);
	}
	else if (ModifierDefinition const* modDef = dynamic_cast<decltype(modDef)>(&_callableDeclaration))
		if (modDef->isImplemented())
			modDef->body().accept(*this);

	return false;
}

void ImmutableValidator::analyseVariableReference(VariableDeclaration const& _variableReference, Expression const& _expression)
{
	if (!_variableReference.isStateVariable() || !_variableReference.immutable())
		return;

	// If this is not an ordinary assignment, we write and read at the same time.
	bool write = _expression.annotation().willBeWrittenTo;
	bool read = !_expression.annotation().willBeWrittenTo || !_expression.annotation().lValueOfOrdinaryAssignment;
	if (write)
	{
		if (!m_currentConstructor)
			m_errorReporter.typeError(
				1581_error,
				_expression.location(),
				"Cannot write to immutable here: Immutable variables can only be initialized inline or assigned directly in the constructor."
			);
		else if (m_currentConstructor->annotation().contract->id() != _variableReference.annotation().contract->id())
			m_errorReporter.typeError(
				7484_error,
				_expression.location(),
				"Cannot write to immutable here: Immutable variables must be initialized in the constructor of the contract they are defined in."
			);
		else if (m_inLoop)
			m_errorReporter.typeError(
				6672_error,
				_expression.location(),
				"Cannot write to immutable here: Immutable variables cannot be initialized inside a loop."
			);
		else if (m_inBranch)
			m_errorReporter.typeError(
				4599_error,
				_expression.location(),
				"Cannot write to immutable here: Immutable variables cannot be initialized inside an if statement."
			);
		else if (m_inTryStatement)
			m_errorReporter.typeError(
					4130_error,
					_expression.location(),
					"Cannot write to immutable here: Immutable variables cannot be initialized inside a try/catch statement."
			);
		else if (m_initializedStateVariables.count(&_variableReference))
		{
			if (!read)
				m_errorReporter.typeError(
					1574_error,
					_expression.location(),
					"Immutable state variable already initialized."
				);
			else
				m_errorReporter.typeError(
					2718_error,
					_expression.location(),
					"Immutable variables cannot be modified after initialization."
				);
		}
		else if (read)
			m_errorReporter.typeError(
				3969_error,
				_expression.location(),
				"Immutable variables must be initialized using an assignment."
			);
		m_initializedStateVariables.emplace(&_variableReference);
	}
	if (
		read &&
		m_inCreationContext &&
		!m_initializedStateVariables.count(&_variableReference)
	)
		m_errorReporter.typeError(
			7733_error,
			_expression.location(),
			"Immutable variables cannot be read before they are initialized."
		);
}

void ImmutableValidator::checkAllVariablesInitialized(solidity::langutil::SourceLocation const& _location)
{
	for (ContractDefinition const* contract: m_mostDerivedContract.annotation().linearizedBaseContracts | ranges::views::reverse)
	{
		for (VariableDeclaration const* varDecl: contract->stateVariables())
			if (varDecl->immutable())
				if (!util::contains(m_initializedStateVariables, varDecl))
					m_errorReporter.typeError(
						2658_error,
						_location,
						solidity::langutil::SecondarySourceLocation().append("Not initialized: ", varDecl->location()),
						"Construction control flow ends without initializing all immutable state variables."
					);

		// Don't check further than the current c'tors contract
		if (contract == m_currentConstructorContract)
			break;
	}
}

void ImmutableValidator::visitCallableIfNew(Declaration const& _declaration)
{
	CallableDeclaration const* _callable = dynamic_cast<CallableDeclaration const*>(&_declaration);
	solAssert(_callable != nullptr, "");

	if (m_visitedCallables.emplace(_callable).second)
		_declaration.accept(*this);
}
