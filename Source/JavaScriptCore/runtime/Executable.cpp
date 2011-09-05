/*
 * Copyright (C) 2009, 2010 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "Executable.h"

#include "BytecodeGenerator.h"
#include "CodeBlock.h"
#include "JIT.h"
#include "Parser.h"
#include "UStringBuilder.h"
#include "Vector.h"

#if ENABLE(DFG_JIT)
#include "DFGByteCodeParser.h"
#include "DFGJITCompiler.h"
#endif

namespace JSC {

const ClassInfo ExecutableBase::s_info = { "Executable", 0, 0, 0 };

#if ENABLE(JIT)
#if ENABLE(DFG_JIT)
static bool tryDFGCompile(ExecState* exec, CodeBlock* codeBlock, JITCode& jitCode)
{
#if ENABLE(DFG_JIT_RESTRICTIONS)
    // FIXME: No flow control yet supported, don't bother scanning the bytecode if there are any jump targets.
    if (codeBlock->numberOfJumpTargets())
        return false;
#endif // ENABLE(DFG_JIT_RESTRICTIONS)

    JSGlobalData* globalData = &exec->globalData();
    DFG::Graph dfg(codeBlock->m_numParameters, codeBlock->m_numVars);
    if (!parse(dfg, globalData, codeBlock))
        return false;

    DFG::JITCompiler dataFlowJIT(globalData, dfg, codeBlock);
    dataFlowJIT.compile(jitCode);
    return true;
}

static bool tryDFGCompileFunction(ExecState* exec, ExecState* calleeArgsExec, CodeBlock* codeBlock, JITCode& jitCode, MacroAssemblerCodePtr& jitCodeWithArityCheck)
{
#if ENABLE(DFG_JIT_RESTRICTIONS)
    // FIXME: No flow control yet supported, don't bother scanning the bytecode if there are any jump targets.
    if (codeBlock->numberOfJumpTargets())
        return false;
#endif // ENABLE(DFG_JIT_RESTRICTIONS)

    JSGlobalData* globalData = &exec->globalData();
    DFG::Graph dfg(codeBlock->m_numParameters, codeBlock->m_numVars);
    if (!parse(dfg, globalData, codeBlock))
        return false;

    if (calleeArgsExec)
        dfg.predictArgumentTypes(calleeArgsExec);

    DFG::JITCompiler dataFlowJIT(globalData, dfg, codeBlock);
    dataFlowJIT.compileFunction(jitCode, jitCodeWithArityCheck);
    return true;
}
#else // ENABLE(DFG_JIT)
static bool tryDFGCompile(ExecState*, CodeBlock*, JITCode&) { return false; }
static bool tryDFGCompileFunction(ExecState*, ExecState*, CodeBlock*, JITCode&, MacroAssemblerCodePtr&) { return false; }
#endif // ENABLE(DFG_JIT)
#endif // ENABLE(JIT)
    
void ExecutableBase::clearCode()
{
#if ENABLE(JIT)
    m_jitCodeForCall.clear();
    m_jitCodeForConstruct.clear();
    m_jitCodeForCallWithArityCheck = MacroAssemblerCodePtr();
    m_jitCodeForConstructWithArityCheck = MacroAssemblerCodePtr();
#endif
    m_numParametersForCall = NUM_PARAMETERS_NOT_COMPILED;
    m_numParametersForConstruct = NUM_PARAMETERS_NOT_COMPILED;
}

class ExecutableFinalizer : public WeakHandleOwner {
    virtual void finalize(Handle<Unknown> handle, void*)
    {
        Weak<ExecutableBase> executable(Weak<ExecutableBase>::Adopt, handle);
        executable->clearCode();
    }
};

WeakHandleOwner* ExecutableBase::executableFinalizer()
{
    DEFINE_STATIC_LOCAL(ExecutableFinalizer, finalizer, ());
    return &finalizer;
}

const ClassInfo NativeExecutable::s_info = { "NativeExecutable", &ExecutableBase::s_info, 0, 0 };

NativeExecutable::~NativeExecutable()
{
}

const ClassInfo ScriptExecutable::s_info = { "ScriptExecutable", &ExecutableBase::s_info, 0, 0 };

const ClassInfo EvalExecutable::s_info = { "EvalExecutable", &ScriptExecutable::s_info, 0, 0 };

EvalExecutable::EvalExecutable(ExecState* exec, const SourceCode& source, bool inStrictContext)
    : ScriptExecutable(exec->globalData().evalExecutableStructure.get(), exec, source, inStrictContext)
{
}

EvalExecutable::~EvalExecutable()
{
}

const ClassInfo ProgramExecutable::s_info = { "ProgramExecutable", &ScriptExecutable::s_info, 0, 0 };

ProgramExecutable::ProgramExecutable(ExecState* exec, const SourceCode& source)
    : ScriptExecutable(exec->globalData().programExecutableStructure.get(), exec, source, false)
{
}

ProgramExecutable::~ProgramExecutable()
{
}

const ClassInfo FunctionExecutable::s_info = { "FunctionExecutable", &ScriptExecutable::s_info, 0, 0 };

FunctionExecutable::FunctionExecutable(JSGlobalData& globalData, const Identifier& name, const SourceCode& source, bool forceUsesArguments, FunctionParameters* parameters, bool inStrictContext, int firstLine, int lastLine)
    : ScriptExecutable(globalData.functionExecutableStructure.get(), globalData, source, inStrictContext)
    , m_numCapturedVariables(0)
    , m_forceUsesArguments(forceUsesArguments)
    , m_parameters(parameters)
    , m_name(name)
    , m_symbolTable(0)
{
    finishCreation(globalData, name, firstLine, lastLine);
}

FunctionExecutable::FunctionExecutable(ExecState* exec, const Identifier& name, const SourceCode& source, bool forceUsesArguments, FunctionParameters* parameters, bool inStrictContext, int firstLine, int lastLine)
    : ScriptExecutable(exec->globalData().functionExecutableStructure.get(), exec, source, inStrictContext)
    , m_numCapturedVariables(0)
    , m_forceUsesArguments(forceUsesArguments)
    , m_parameters(parameters)
    , m_name(name)
    , m_symbolTable(0)
{
    m_firstLine = firstLine;
    m_lastLine = lastLine;
    finishCreation(exec->globalData(), name, firstLine, lastLine);
}


JSObject* EvalExecutable::compileInternal(ExecState* exec, ScopeChainNode* scopeChainNode)
{
    JSObject* exception = 0;
    JSGlobalData* globalData = &exec->globalData();
    JSGlobalObject* lexicalGlobalObject = exec->lexicalGlobalObject();
    if (!lexicalGlobalObject->evalEnabled())
        return throwError(exec, createEvalError(exec, "Eval is disabled"));
    RefPtr<EvalNode> evalNode = globalData->parser->parse<EvalNode>(lexicalGlobalObject, lexicalGlobalObject->debugger(), exec, m_source, 0, isStrictMode() ? JSParseStrict : JSParseNormal, &exception);
    if (!evalNode) {
        ASSERT(exception);
        return exception;
    }
    recordParse(evalNode->features(), evalNode->hasCapturedVariables(), evalNode->lineNo(), evalNode->lastLine());

    JSGlobalObject* globalObject = scopeChainNode->globalObject.get();

    ASSERT(!m_evalCodeBlock);
    m_evalCodeBlock = adoptPtr(new EvalCodeBlock(this, globalObject, source().provider(), scopeChainNode->localDepth()));
    OwnPtr<BytecodeGenerator> generator(adoptPtr(new BytecodeGenerator(evalNode.get(), scopeChainNode, m_evalCodeBlock->symbolTable(), m_evalCodeBlock.get())));
    if ((exception = generator->generate())) {
        m_evalCodeBlock.clear();
        evalNode->destroyData();
        return exception;
    }

    evalNode->destroyData();

#if ENABLE(JIT)
    if (exec->globalData().canUseJIT()) {
        bool dfgCompiled = tryDFGCompile(exec, m_evalCodeBlock.get(), m_jitCodeForCall);
        if (!dfgCompiled)
            m_jitCodeForCall = JIT::compile(scopeChainNode->globalData, m_evalCodeBlock.get());
#if !ENABLE(OPCODE_SAMPLING)
        if (!BytecodeGenerator::dumpsGeneratedCode())
            m_evalCodeBlock->discardBytecode();
#endif
    }
#endif

#if ENABLE(JIT)
#if ENABLE(INTERPRETER)
    if (!m_jitCodeForCall)
        Heap::heap(this)->reportExtraMemoryCost(sizeof(*m_evalCodeBlock));
    else
#endif
    Heap::heap(this)->reportExtraMemoryCost(sizeof(*m_evalCodeBlock) + m_jitCodeForCall.size());
#else
    Heap::heap(this)->reportExtraMemoryCost(sizeof(*m_evalCodeBlock));
#endif

    return 0;
}

void EvalExecutable::visitChildren(SlotVisitor& visitor)
{
    ASSERT_GC_OBJECT_INHERITS(this, &s_info);
    COMPILE_ASSERT(StructureFlags & OverridesVisitChildren, OverridesVisitChildrenWithoutSettingFlag);
    ASSERT(structure()->typeInfo().overridesVisitChildren());
    ScriptExecutable::visitChildren(visitor);
    if (m_evalCodeBlock)
        m_evalCodeBlock->visitAggregate(visitor);
}

void EvalExecutable::unlinkCalls()
{
#if ENABLE(JIT)
    if (!m_jitCodeForCall)
        return;
    ASSERT(m_evalCodeBlock);
    m_evalCodeBlock->unlinkCalls();
#endif
}

void EvalExecutable::clearCode()
{
    if (m_evalCodeBlock) {
        m_evalCodeBlock->clearEvalCache();
        m_evalCodeBlock.clear();
    }
    Base::clearCode();
}

JSObject* ProgramExecutable::checkSyntax(ExecState* exec)
{
    JSObject* exception = 0;
    JSGlobalData* globalData = &exec->globalData();
    JSGlobalObject* lexicalGlobalObject = exec->lexicalGlobalObject();
    RefPtr<ProgramNode> programNode = globalData->parser->parse<ProgramNode>(lexicalGlobalObject, lexicalGlobalObject->debugger(), exec, m_source, 0, JSParseNormal, &exception);
    if (programNode)
        return 0;
    ASSERT(exception);
    return exception;
}

JSObject* ProgramExecutable::compileInternal(ExecState* exec, ScopeChainNode* scopeChainNode)
{
    ASSERT(!m_programCodeBlock);

    JSObject* exception = 0;
    JSGlobalData* globalData = &exec->globalData();
    JSGlobalObject* lexicalGlobalObject = exec->lexicalGlobalObject();
    RefPtr<ProgramNode> programNode = globalData->parser->parse<ProgramNode>(lexicalGlobalObject, lexicalGlobalObject->debugger(), exec, m_source, 0, isStrictMode() ? JSParseStrict : JSParseNormal, &exception);
    if (!programNode) {
        ASSERT(exception);
        return exception;
    }
    recordParse(programNode->features(), programNode->hasCapturedVariables(), programNode->lineNo(), programNode->lastLine());

    JSGlobalObject* globalObject = scopeChainNode->globalObject.get();
    
    m_programCodeBlock = adoptPtr(new ProgramCodeBlock(this, GlobalCode, globalObject, source().provider()));
    OwnPtr<BytecodeGenerator> generator(adoptPtr(new BytecodeGenerator(programNode.get(), scopeChainNode, &globalObject->symbolTable(), m_programCodeBlock.get())));
    if ((exception = generator->generate())) {
        m_programCodeBlock.clear();
        programNode->destroyData();
        return exception;
    }

    programNode->destroyData();

#if ENABLE(JIT)
    if (exec->globalData().canUseJIT()) {
        bool dfgCompiled = tryDFGCompile(exec, m_programCodeBlock.get(), m_jitCodeForCall);
        if (!dfgCompiled)
            m_jitCodeForCall = JIT::compile(scopeChainNode->globalData, m_programCodeBlock.get());
#if !ENABLE(OPCODE_SAMPLING)
        if (!BytecodeGenerator::dumpsGeneratedCode())
            m_programCodeBlock->discardBytecode();
#endif
    }
#endif

#if ENABLE(JIT)
#if ENABLE(INTERPRETER)
    if (!m_jitCodeForCall)
        Heap::heap(this)->reportExtraMemoryCost(sizeof(*m_programCodeBlock));
    else
#endif
        Heap::heap(this)->reportExtraMemoryCost(sizeof(*m_programCodeBlock) + m_jitCodeForCall.size());
#else
    Heap::heap(this)->reportExtraMemoryCost(sizeof(*m_programCodeBlock));
#endif

    return 0;
}

void ProgramExecutable::unlinkCalls()
{
#if ENABLE(JIT)
    if (!m_jitCodeForCall)
        return;
    ASSERT(m_programCodeBlock);
    m_programCodeBlock->unlinkCalls();
#endif
}

void ProgramExecutable::visitChildren(SlotVisitor& visitor)
{
    ASSERT_GC_OBJECT_INHERITS(this, &s_info);
    COMPILE_ASSERT(StructureFlags & OverridesVisitChildren, OverridesVisitChildrenWithoutSettingFlag);
    ASSERT(structure()->typeInfo().overridesVisitChildren());
    ScriptExecutable::visitChildren(visitor);
    if (m_programCodeBlock)
        m_programCodeBlock->visitAggregate(visitor);
}

void ProgramExecutable::clearCode()
{
    if (m_programCodeBlock) {
        m_programCodeBlock->clearEvalCache();
        m_programCodeBlock.clear();
    }
    Base::clearCode();
}

JSObject* FunctionExecutable::compileForCallInternal(ExecState* exec, ScopeChainNode* scopeChainNode, ExecState* calleeArgsExec)
{
    JSObject* exception = 0;
    JSGlobalData* globalData = scopeChainNode->globalData;
    RefPtr<FunctionBodyNode> body = globalData->parser->parse<FunctionBodyNode>(exec->lexicalGlobalObject(), 0, 0, m_source, m_parameters.get(), isStrictMode() ? JSParseStrict : JSParseNormal, &exception);
    if (!body) {
        ASSERT(exception);
        return exception;
    }
    if (m_forceUsesArguments)
        body->setUsesArguments();
    body->finishParsing(m_parameters, m_name);
    recordParse(body->features(), body->hasCapturedVariables(), body->lineNo(), body->lastLine());

    JSGlobalObject* globalObject = scopeChainNode->globalObject.get();

    ASSERT(!m_codeBlockForCall);
    m_codeBlockForCall = adoptPtr(new FunctionCodeBlock(this, FunctionCode, globalObject, source().provider(), source().startOffset(), false));
    OwnPtr<BytecodeGenerator> generator(adoptPtr(new BytecodeGenerator(body.get(), scopeChainNode, m_codeBlockForCall->symbolTable(), m_codeBlockForCall.get())));
    if ((exception = generator->generate())) {
        m_codeBlockForCall.clear();
        body->destroyData();
        return exception;
    }

    m_numParametersForCall = m_codeBlockForCall->m_numParameters;
    ASSERT(m_numParametersForCall);
    m_numCapturedVariables = m_codeBlockForCall->m_numCapturedVars;
    m_symbolTable = m_codeBlockForCall->sharedSymbolTable();

    body->destroyData();

#if ENABLE(JIT)
    if (exec->globalData().canUseJIT()) {
        bool dfgCompiled = tryDFGCompileFunction(exec, calleeArgsExec, m_codeBlockForCall.get(), m_jitCodeForCall, m_jitCodeForCallWithArityCheck);
        if (!dfgCompiled)
            m_jitCodeForCall = JIT::compile(scopeChainNode->globalData, m_codeBlockForCall.get(), &m_jitCodeForCallWithArityCheck);

#if !ENABLE(OPCODE_SAMPLING)
        if (!BytecodeGenerator::dumpsGeneratedCode())
            m_codeBlockForCall->discardBytecode();
#endif
    }
#else
    UNUSED_PARAM(calleeArgsExec);
#endif

#if ENABLE(JIT)
#if ENABLE(INTERPRETER)
    if (!m_jitCodeForCall)
        Heap::heap(this)->reportExtraMemoryCost(sizeof(*m_codeBlockForCall));
    else
#endif
        Heap::heap(this)->reportExtraMemoryCost(sizeof(*m_codeBlockForCall) + m_jitCodeForCall.size());
#else
    Heap::heap(this)->reportExtraMemoryCost(sizeof(*m_codeBlockForCall));
#endif

    return 0;
}

JSObject* FunctionExecutable::compileForConstructInternal(ExecState* exec, ScopeChainNode* scopeChainNode)
{
    JSObject* exception = 0;
    JSGlobalData* globalData = scopeChainNode->globalData;
    RefPtr<FunctionBodyNode> body = globalData->parser->parse<FunctionBodyNode>(exec->lexicalGlobalObject(), 0, 0, m_source, m_parameters.get(), isStrictMode() ? JSParseStrict : JSParseNormal, &exception);
    if (!body) {
        ASSERT(exception);
        return exception;
    }
    if (m_forceUsesArguments)
        body->setUsesArguments();
    body->finishParsing(m_parameters, m_name);
    recordParse(body->features(), body->hasCapturedVariables(), body->lineNo(), body->lastLine());

    JSGlobalObject* globalObject = scopeChainNode->globalObject.get();

    ASSERT(!m_codeBlockForConstruct);
    m_codeBlockForConstruct = adoptPtr(new FunctionCodeBlock(this, FunctionCode, globalObject, source().provider(), source().startOffset(), true));
    OwnPtr<BytecodeGenerator> generator(adoptPtr(new BytecodeGenerator(body.get(), scopeChainNode, m_codeBlockForConstruct->symbolTable(), m_codeBlockForConstruct.get())));
    if ((exception = generator->generate())) {
        m_codeBlockForConstruct.clear();
        body->destroyData();
        return exception;
    }

    m_numParametersForConstruct = m_codeBlockForConstruct->m_numParameters;
    ASSERT(m_numParametersForConstruct);
    m_numCapturedVariables = m_codeBlockForConstruct->m_numCapturedVars;
    m_symbolTable = m_codeBlockForConstruct->sharedSymbolTable();

    body->destroyData();

#if ENABLE(JIT)
    if (exec->globalData().canUseJIT()) {
        m_jitCodeForConstruct = JIT::compile(scopeChainNode->globalData, m_codeBlockForConstruct.get(), &m_jitCodeForConstructWithArityCheck);
#if !ENABLE(OPCODE_SAMPLING)
        if (!BytecodeGenerator::dumpsGeneratedCode())
            m_codeBlockForConstruct->discardBytecode();
#endif
    }
#endif

#if ENABLE(JIT)
#if ENABLE(INTERPRETER)
    if (!m_jitCodeForConstruct)
        Heap::heap(this)->reportExtraMemoryCost(sizeof(*m_codeBlockForConstruct));
    else
#endif
    Heap::heap(this)->reportExtraMemoryCost(sizeof(*m_codeBlockForConstruct) + m_jitCodeForConstruct.size());
#else
    Heap::heap(this)->reportExtraMemoryCost(sizeof(*m_codeBlockForConstruct));
#endif

    return 0;
}

void FunctionExecutable::visitChildren(SlotVisitor& visitor)
{
    ASSERT_GC_OBJECT_INHERITS(this, &s_info);
    COMPILE_ASSERT(StructureFlags & OverridesVisitChildren, OverridesVisitChildrenWithoutSettingFlag);
    ASSERT(structure()->typeInfo().overridesVisitChildren());
    ScriptExecutable::visitChildren(visitor);
    if (m_nameValue)
        visitor.append(&m_nameValue);
    if (m_codeBlockForCall)
        m_codeBlockForCall->visitAggregate(visitor);
    if (m_codeBlockForConstruct)
        m_codeBlockForConstruct->visitAggregate(visitor);
}

void FunctionExecutable::discardCode()
{
#if ENABLE(JIT)
    // These first two checks are to handle the rare case where
    // we are trying to evict code for a function during its
    // codegen.
    if (!m_jitCodeForCall && m_codeBlockForCall)
        return;
    if (!m_jitCodeForConstruct && m_codeBlockForConstruct)
        return;
#endif
    clearCode();
}

void FunctionExecutable::clearCode()
{
    if (m_codeBlockForCall) {
        m_codeBlockForCall->clearEvalCache();
        m_codeBlockForCall.clear();
    }
    if (m_codeBlockForConstruct) {
        m_codeBlockForConstruct->clearEvalCache();
        m_codeBlockForConstruct.clear();
    }
    Base::clearCode();
}

void FunctionExecutable::unlinkCalls()
{
#if ENABLE(JIT)
    if (!!m_jitCodeForCall) {
        ASSERT(m_codeBlockForCall);
        m_codeBlockForCall->unlinkCalls();
    }
    if (!!m_jitCodeForConstruct) {
        ASSERT(m_codeBlockForConstruct);
        m_codeBlockForConstruct->unlinkCalls();
    }
#endif
}

FunctionExecutable* FunctionExecutable::fromGlobalCode(const Identifier& functionName, ExecState* exec, Debugger* debugger, const SourceCode& source, JSObject** exception)
{
    JSGlobalObject* lexicalGlobalObject = exec->lexicalGlobalObject();
    RefPtr<ProgramNode> program = exec->globalData().parser->parse<ProgramNode>(lexicalGlobalObject, debugger, exec, source, 0, JSParseNormal, exception);
    if (!program) {
        ASSERT(*exception);
        return 0;
    }

    // Uses of this function that would not result in a single function expression are invalid.
    StatementNode* exprStatement = program->singleStatement();
    ASSERT(exprStatement);
    ASSERT(exprStatement->isExprStatement());
    ExpressionNode* funcExpr = static_cast<ExprStatementNode*>(exprStatement)->expr();
    ASSERT(funcExpr);
    ASSERT(funcExpr->isFuncExprNode());
    FunctionBodyNode* body = static_cast<FuncExprNode*>(funcExpr)->body();
    ASSERT(body);

    return FunctionExecutable::create(exec->globalData(), functionName, body->source(), body->usesArguments(), body->parameters(), body->isStrictMode(), body->lineNo(), body->lastLine());
}

UString FunctionExecutable::paramString() const
{
    FunctionParameters& parameters = *m_parameters;
    UStringBuilder builder;
    for (size_t pos = 0; pos < parameters.size(); ++pos) {
        if (!builder.isEmpty())
            builder.append(", ");
        builder.append(parameters[pos].ustring());
    }
    return builder.toUString();
}

}