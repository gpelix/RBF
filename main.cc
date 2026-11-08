//===- main.cc ------------------------------------------------*- C++ -*-===//
//
// This file contains the main routine of the rbfc (Rome brainf*** compiler)
//
//===--------------------------------------------------------------------===//
#include <iostream>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <utility>
#include <optional>
#include <array>
#include <filesystem>

#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <cerrno>
#include <cstdbool>

#include <unistd.h>
#include <getopt.h>

#include <sys/wait.h>

#ifndef TEMPORARY_DIRECTORY
#define TEMPORARY_DIRECTORY	"/tmp/"
#endif

#ifndef MAXPATHLEN
#ifdef PATH_MAX
#define MAXPATHLEN      PATH_MAX
#else
#define MAXPATHLEN      1024
#endif
#endif

// #include "llvm/ADT/None.h" Rimosso nella 16
#include "llvm/IR/Attributes.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
// #include "llvm/Support/Host.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"


#ifdef _HAS_GNU_HOT
#define __hot	[[gnu::hot]]
#else
#define __hot
#endif

#ifndef _VERSION
#error _VERSION must be defined
#endif

enum class OptLevel {
  O0,
  O1,
  O2,
  O3,
  Os,
  Oz
};

static llvm::OptimizationLevel toLLVMOptLevel(OptLevel opt_level)
{
  switch (opt_level) {
  case OptLevel::O0: return llvm::OptimizationLevel::O0;
  case OptLevel::O1: return llvm::OptimizationLevel::O1;
  case OptLevel::O2: return llvm::OptimizationLevel::O2;
  case OptLevel::O3: return llvm::OptimizationLevel::O3;

  case OptLevel::Os:
  case OptLevel::Oz:
      return llvm::OptimizationLevel::O2;
  }

  std::abort();
}

static const char *optLevelName(OptLevel level)
{
    switch (level) {
    case OptLevel::O0: return "O0";
    case OptLevel::O1: return "O1";
    case OptLevel::O2: return "O2";
    case OptLevel::O3: return "O3";
    case OptLevel::Os: return "Os";
    case OptLevel::Oz: return "Oz";
    }
    return "unknown";
}

//===-------------------------------------------------------------------------
// Utilities
//===-------------------------------------------------------------------------
template <typename T, typename U>
inline bool map_has(const std::map<T, U>& m, const T& key)
{
	return m.find(key) != m.end();
}

static std::optional<std::pair<std::string, int>> get_tmpfile()
{

	char tname[MAXPATHLEN];
	int fd;

	(void)snprintf(tname, sizeof(tname),
	    "%s/.XXXXXX", TEMPORARY_DIRECTORY);

	if ((fd = mkstemp(tname)) == -1) {
		std::cerr << "Unable to create temporary file " <<
			strerror(errno) << std::endl;
		return std::optional<std::pair<std::string, int>>();
	}

	return std::optional(std::pair(std::string(tname), fd));
}

// Return -1 on error
int safe_store_atol(const char *str, long *n)
{
	// We could have used a reference instread of a bool *but it seems
	// we are passing a bool by value
	char *endptr;
	*n = strtol(str, &endptr, 10);

	if (*str != '\0' && *endptr == '\0')
		return 0; // Ok
	else
		return -1; // Failure
}

//===-------------------------------------------------------------------------
// Tokens
//===-------------------------------------------------------------------------
enum class TokenType { Char, Eof };

class Token {
	TokenType tt;
	std::string s;
public:
	Token(TokenType tt, const std::string& s) : tt(tt), s(s) {}

	constexpr TokenType getTokType() const { return tt; }
	constexpr const std::string& getString() const { return s; }
	constexpr operator bool() const { return tt != TokenType::Eof; }

	friend std::ostream& operator<<(std::ostream& os, const Token& tok)
		{ os << tok.s; return os; }
};


//===-------------------------------------------------------------------------
// Lexer
//===-------------------------------------------------------------------------
class Lexer {
	std::istream& reader;
	std::deque<Token> next_tokens; // This is a deque for viewed but not got tokens

	Token readTok();
public:
	Lexer(std::istream& reader) : reader(reader) {};

	Token getTok();
	Token lookAhead(size_t n = 0);
};

Token Lexer::readTok()
{
	char c;

	if (reader.eof())
		return Token(TokenType::Eof, "Eof");

	reader.read(&c, 1);
	return Token(TokenType::Char, std::string(1, c));
}

Token Lexer::lookAhead(size_t n)
{
	size_t size_next = next_tokens.size();
	if (n + 1 > size_next) {
		size_t toks_to_take = n + 1 - size_next;
		for (int i = 0; i < (int)toks_to_take; i++)
			next_tokens.push_back(readTok());
	}
	return next_tokens.at(n);
}

Token Lexer::getTok()
{
	if (next_tokens.size() != 0) {
		Token ret = std::move(next_tokens.front());
		next_tokens.pop_front();
		return ret;
	} else
		return readTok();
}

//===-------------------------------------------------------------------------
// Abstract syntax tree
//===-------------------------------------------------------------------------
class CodeGenerator; // Needed by generateCode

namespace Ast {

// Base class for every node pf the AST
class ExprAst {
public:
	virtual ~ExprAst() = default;
	virtual void render(std::ostream&) const = 0;
	virtual void generateCode(CodeGenerator&) const = 0;

	friend std::ostream& operator<<(std::ostream& os, const ExprAst& expr)
		{ expr.render(os); return os; }
};

// It represents loop
class LoopAst : public ExprAst {
	std::unique_ptr<ExprAst> frame;
public:
	LoopAst(std::unique_ptr<ExprAst> frame) : frame(std::move(frame)) {}
	void render(std::ostream& os) const override;
	__hot void generateCode(CodeGenerator&) const override;

};

enum class OpCodes {
	Plus,
	Minus,
	Greater,
	Lower,
	Dot,
	Comma,
};

static std::map<std::string, OpCodes> opcode {
	std::make_pair("+", OpCodes::Plus),
	std::make_pair("-", OpCodes::Minus),
	std::make_pair(">", OpCodes::Greater),
	std::make_pair("<", OpCodes::Lower),
	std::make_pair(".", OpCodes::Dot),
	std::make_pair(",", OpCodes::Comma)
};

static std::map<OpCodes, std::string> inverse {
	std::make_pair(OpCodes::Plus, "+"),
	std::make_pair(OpCodes::Minus, "-"),
	std::make_pair(OpCodes::Greater, ">"),
	std::make_pair(OpCodes::Lower, "<"),
	std::make_pair(OpCodes::Dot, "."),
	std::make_pair(OpCodes::Comma, ",")
};

// It represents a single op +-[]<>,.
class OpAst : public ExprAst {
	OpCodes op;
public:
	OpAst(const std::string& s) : op(opcode.at(s)) {}
	void render(std::ostream& os) const override;
	__hot void generateCode(CodeGenerator&) const override;
};

// It represents a frame of operations
class FrameAst : public ExprAst {
	std::vector<std::unique_ptr<ExprAst>> children;
public:
	template <typename U>
	FrameAst(U&& children) : children(std::forward<U>(children)) {}

	void render(std::ostream& os) const override;
	__hot void generateCode(CodeGenerator&) const override;
};

//===-------------------------------------------------------------------------
// Renderers
//===-------------------------------------------------------------------------
void LoopAst::render(std::ostream& os) const
	{ os << "[" << *frame << "]"; }

void OpAst::render(std::ostream& os) const
	{ os << inverse.at(op); }

void FrameAst::render(std::ostream& os) const
{
	for (const std::unique_ptr<ExprAst>& c : children) {
		os << *c;
	}
}


} /// End of namespace Ast

//===-------------------------------------------------------------------------
// Parser
//===-------------------------------------------------------------------------
class Parser {
	Lexer lexer;
	size_t depth; // Incremented when we get into a loop
protected:
	std::unique_ptr<Ast::ExprAst> parseOp();
	std::unique_ptr<Ast::ExprAst> parseLoop();
	std::unique_ptr<Ast::ExprAst> parseFrame();
public:
	Parser(std::istream& reader) : lexer(reader), depth(0) {}

	std::unique_ptr<Ast::ExprAst> parse() { return parseFrame(); }
};

std::unique_ptr<Ast::ExprAst> Parser::parseOp()
{
	//std::cout << depth << " ParseOp\n";
	// We know it's not eof and it's not [ or ]
	Token tok = lexer.getTok();
	auto str = tok.getString();
	return std::make_unique<Ast::OpAst>(str);
}

std::unique_ptr<Ast::ExprAst> Parser::parseLoop()
{
	//std::cout << depth << " ParseLoop\n";
	(void)lexer.getTok(); // Eat [
	depth++;
	auto inner_frame = parseFrame();
	if (!inner_frame)
		return nullptr;
	auto ret = std::make_unique<Ast::LoopAst>(std::move(inner_frame));
	depth--;
	(void)lexer.getTok(); // Eat ]
	//std::cout << depth << " ENDParseLoop\n";
	return ret;
}

std::unique_ptr<Ast::ExprAst> Parser::parseFrame()
{
	std::vector<std::unique_ptr<Ast::ExprAst>> children;
	//std::cout << depth << " ParseFrame\n";
	while (true) {
		Token tok = lexer.lookAhead();
		if (!tok) {       // EOF found
			if (depth == 0)
				break;
			else {
				std::cerr << "Unexpected EOF found. Unmatched [" << std::endl;
				return nullptr;
			}
		}
		const std::string& tok_string = tok.getString();
		if (tok_string == "]") {
			if (depth == 0) {
				std::cerr << "Found unmatched ]" << std::endl;
				return nullptr;
			} else
				break;
		}
		if (tok_string == "[") {
			auto inner_loop = parseLoop();
			if (!inner_loop)
				return nullptr;
			children.push_back(std::move(inner_loop));
		} else if (map_has(Ast::opcode, tok_string)) {
			children.push_back(parseOp());
		} else {
			// It's just comment
			(void)lexer.getTok(); // Eat it
		}
	}
	//std::cout << depth << " ENDParseFrame\n";
	return std::make_unique<Ast::FrameAst>(std::move(children));
}

//==--------------------------------------------------------------------------
// Optimizer
//==--------------------------------------------------------------------------
llvm::CodeGenOptLevel OptimizationLevel2CodeGenOpt(OptLevel opt_level)
{
  switch (opt_level) {
  case OptLevel::O0:
    return llvm::CodeGenOptLevel::None;

  case OptLevel::O1:
    return llvm::CodeGenOptLevel::Less;

  case OptLevel::O2:
  case OptLevel::Os:
  case OptLevel::Oz:
    return llvm::CodeGenOptLevel::Default;

  case OptLevel::O3:
    return llvm::CodeGenOptLevel::Aggressive;
  }

  std::abort();
}

class Optimizer {
	llvm::OptimizationLevel opt_level;
	llvm::LoopAnalysisManager LAM;
	llvm::FunctionAnalysisManager FAM;
	llvm::CGSCCAnalysisManager CGAM;
	llvm::ModuleAnalysisManager MAM;
	llvm::PassBuilder PB;
	llvm::ModulePassManager MPM;
public:
	Optimizer(llvm::OptimizationLevel opt_level);
	void run(llvm::Module& Module);
};

Optimizer::Optimizer(llvm::OptimizationLevel opt_level)
	: opt_level(opt_level)
{
	// Register all the basic analyses with the managers.
	PB.registerModuleAnalyses(MAM);
	PB.registerCGSCCAnalyses(CGAM);
	PB.registerFunctionAnalyses(FAM);
	PB.registerLoopAnalyses(LAM);
	PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

	MPM = PB.buildPerModuleDefaultPipeline(opt_level);
}

inline void Optimizer::run(llvm::Module& Module)
{
	MPM.run(Module, MAM);
}

//===-------------------------------------------------------------------------
// CodeGenerator
//===-------------------------------------------------------------------------
class CodeGenerator {
	llvm::LLVMContext context;
	llvm::IRBuilder<> builder;
	llvm::Module Module;
	llvm::AllocaInst *data_ptr; // This will hold the data_ptr variable.
                                // Maybe uninitialized
	llvm::Function *Main;
	Optimizer optimizer;
	bool generated;

	void dumpLlvmIR(llvm::raw_ostream& OS) const
	{
		OS << Module;
		OS.flush();
	}
public:
	CodeGenerator(OptLevel, std::unique_ptr<Ast::ExprAst>&, long data_length);
	constexpr inline llvm::IRBuilder<>& getBuilder() { return builder; }
	constexpr inline llvm::LLVMContext& getContext() { return context; }
	constexpr inline llvm::AllocaInst* getDataPtr() { return data_ptr; }
	constexpr inline llvm::Module& getModule() { return Module; }
	constexpr inline llvm::Function* getMain() { assert(Main); return Main; }
	void emitLlvmIR(int fd);

	friend std::ostream& operator<<(std::ostream& os, const CodeGenerator& cg)
	{
		if (!cg.generated) {
			os << "Nothing generated yet. No IR available";
			return os;
		}

		std::string str;

		// Rewrite for without ostream to write on fd
		llvm::raw_string_ostream OS(str);
		cg.dumpLlvmIR(OS);

		os << str;

		return os;
	}
};

void CodeGenerator::emitLlvmIR(int fd)
{
	if (!generated) {
		std::cerr << "Nothing generated yet. No IR available" << std::endl;
		return;
	}

	llvm::raw_fd_ostream OS(fd, false, false, llvm::raw_ostream::OStreamKind::OK_FDStream);
	OS << Module;

	dumpLlvmIR(OS);
}

CodeGenerator::CodeGenerator(OptLevel opt_level,
                             std::unique_ptr<Ast::ExprAst>& ast,
                             long data_length) :
                                                           builder(context),
                                                           Module("bfc", context),
                                                           data_ptr(nullptr),
                                                           optimizer(toLLVMOptLevel(opt_level)),
                                                           generated(false)
{
	// In our program we will have a global
	// array called data
	// a main functon in which we allocate a movable
	// pointer (data_ptr) pointing into data

  llvm::Triple target_triple(llvm::sys::getDefaultTargetTriple());
  Module.setTargetTriple(target_triple);
	
	// Declare external functions for IO
	// Putchar
	std::array<llvm::Type *, 1> putchar_args = { llvm::Type::getInt32Ty(context) };
	llvm::FunctionType *putchar_type =
		llvm::FunctionType::get(
			llvm::Type::getVoidTy(context), // We don't care about the result
			putchar_args,
			false);

	llvm::Function::Create(putchar_type,
	                       llvm::Function::ExternalLinkage,
	                       "putchar",
	                       Module);

	// Getchar
	llvm::FunctionType *getchar_type =
		llvm::FunctionType::get(
			llvm::Type::getInt32Ty(context),
			false);

	llvm::Function::Create(getchar_type,
	                       llvm::Function::ExternalLinkage,
	                       "getchar",
	                       Module);

	// We have two different versions of data. In one we can allocate it as a global
	// array. Otherwise we could also use the stack (see below)
	// Declare global array
//#if 0
	llvm::GlobalVariable *data = new llvm::GlobalVariable(Module,
		/*Ty=*/llvm::ArrayType::get(llvm::Type::getInt8Ty(context), data_length),
		/*isConstant=*/false,
		/*LinkageTypes=*/llvm::GlobalValue::LinkageTypes::InternalLinkage,
		/*Initializer=*/llvm::Constant::getNullValue(
                	llvm::ArrayType::get(llvm::Type::getInt8Ty(context), data_length)),
		/*Name=*/"data");
//#endif

	// Create main
	llvm::FunctionType *main_type =
		llvm::FunctionType::get(llvm::Type::getVoidTy(context), false);

	Main = llvm::Function::Create(main_type,
	                              llvm::Function::ExternalLinkage,
	                              "main",
	                              Module);

  if (opt_level == OptLevel::Os) {
    Main->addFnAttr(llvm::Attribute::OptimizeForSize);
  } else if (opt_level == OptLevel::Oz) {
    Main->addFnAttr(llvm::Attribute::OptimizeForSize);
    Main->addFnAttr(llvm::Attribute::MinSize);
  }

	// First block
	llvm::BasicBlock *basic_block =
		llvm::BasicBlock::Create(context, "entry", Main);

	builder.SetInsertPoint(basic_block);

#if 0
	// Stack based
	llvm::AllocaInst *data = builder.CreateAlloca(
		llvm::ArrayType::get(llvm::Type::getInt8Ty(context), data_length),
		nullptr,
		"data");

	for (int i = 0; i < data_length; i++) {
		std::array<llvm::Value *, 1> idx_list = {
	//		llvm::ConstantInt::get(context, llvm::APInt(32, 0, false)),
			llvm::ConstantInt::get(context, llvm::APInt(32, i, false))
		};

		llvm::Value *added_temp = builder.CreateGEP(
			llvm::Type::getInt8Ty(context),
			data,
			idx_list,
			"addedtemp");
		builder.CreateStore(llvm::ConstantInt::get(context, llvm::APInt(8, 0, false)), added_temp);
	}
#endif

	// Declare pointer to global array
	data_ptr = builder.CreateAlloca(
		llvm::PointerType::getUnqual(context), nullptr, "data.ptr");
	builder.CreateStore(data, data_ptr);

	// Generate all the code
	ast->generateCode(*this);

	// ret void
	builder.CreateRetVoid();

	// std::cout << "Verifying\n";
	llvm::verifyFunction(*Main); // XXX: We could remove this later

	// Optimize
	//std::cout << "Optimizing" << std::endl;
	optimizer.run(Module);
	//std::cout << "EndOptimizing" << std::endl;

	generated = true;
}

//===-------------------------------------------------------------------------
// Code generators
//===-------------------------------------------------------------------------
// Create a load of the memory holding the pointer
inline llvm::Value *loadDataPtr(CodeGenerator& gen)
{
	return gen.getBuilder().CreateLoad(
		gen.getDataPtr()->getAllocatedType(),
	       	gen.getDataPtr(),
		"loaded.ptr.");
}

// Read from the data pointer
inline llvm::Value *loadFromDataPtr(CodeGenerator& gen,
                                    llvm::Value *loaded_data_ptr)
{
	return gen.getBuilder().CreateLoad(
		llvm::Type::getInt8Ty(gen.getContext()),
		loaded_data_ptr,
		"loaded.i8.");
}

// Implement bf +/- ops
inline void generateBfAdd(CodeGenerator& gen, bool sub)
{
	auto& builder = gen.getBuilder();
	auto& context = gen.getContext();

	llvm::Value *loaded_ptr = loadDataPtr(gen);
	llvm::Value *loaded_char = loadFromDataPtr(gen, loaded_ptr);

	// compute
	llvm::Value *added_temp = builder.CreateAdd(
		loaded_char,
		llvm::ConstantInt::get(context, llvm::APInt(8, sub ? -1 : 1, false)),
		"add.res.");

	builder.CreateStore(added_temp, loaded_ptr);
}

// Implement bf </>
inline void generateBfInc(CodeGenerator& gen, bool sub)
{
	auto& builder = gen.getBuilder();
	auto data_ptr = gen.getDataPtr();
	auto& context = gen.getContext();

	llvm::Value *loaded_ptr = loadDataPtr(gen);

	// Use getelement ptr
	// compute // Maybe different
	std::array<llvm::Value *, 1> idx_list = {
		llvm::ConstantInt::get(context, llvm::APInt(8, sub ? -1 : 1, false))
	};

	llvm::Value *added_temp = builder.CreateGEP(
		llvm::Type::getInt8Ty(context),
		loaded_ptr,
		idx_list,
		"gep.res.");

	builder.CreateStore(added_temp, data_ptr);
}

inline void generateBfComma(CodeGenerator& gen)
{
	auto& builder = gen.getBuilder();
	auto& context = gen.getContext();
	auto& Module = gen.getModule();

	llvm::Function *Getchar = Module.getFunction("getchar");
	assert(Getchar);
	llvm::Value *got_i32 =
	       	builder.CreateCall(Getchar, {}, "getchar.res."); // if llvm::None gives problems
		                                                         // in the future use std::nullopt

	llvm::Value *got_i8 = builder.CreateIntCast(
		got_i32, llvm::Type::getInt8Ty(context),
		/*isSigned=*/false, "getchar.res.2.i8.");

	llvm::Value *loaded_ptr = loadDataPtr(gen);

	builder.CreateStore(got_i8, loaded_ptr);
}

inline void generateBfDot(CodeGenerator& gen)
{
	auto& builder = gen.getBuilder();
	auto& context = gen.getContext();
	auto& Module = gen.getModule();

	llvm::Value *loaded_ptr = loadDataPtr(gen);
	llvm::Value *loaded_i8 = loadFromDataPtr(gen, loaded_ptr);

	llvm::Value *arg = builder.CreateIntCast(
		loaded_i8, llvm::Type::getInt32Ty(context),
	       	/*isSigned=*/false, "loaded.i8.2.i32.");
	llvm::Function *Putchar = Module.getFunction("putchar");
	assert(Putchar);

	std::array<llvm::Value *, 1> args = { arg };
	builder.CreateCall(Putchar, args);
}

__hot void Ast::OpAst::generateCode(CodeGenerator& gen) const
{
	switch (op) {
	case OpCodes::Plus: generateBfAdd(gen, false); break;
	case OpCodes::Minus: generateBfAdd(gen, true); break;
	case OpCodes::Greater: generateBfInc(gen, false); break;
	case OpCodes::Lower: generateBfInc(gen, true); break;
	case OpCodes::Dot: generateBfDot(gen); break;
	case OpCodes::Comma: generateBfComma(gen); break;
	default: abort(/*"Shouldn't be here"*/);
	}
}

__hot void Ast::LoopAst::generateCode(CodeGenerator& gen) const
{
	auto& builder = gen.getBuilder();
	auto& context = gen.getContext();
	auto *Main = gen.getMain();

	llvm::Value *loaded_ptr = loadDataPtr(gen);
	llvm::Value *loaded_i8 = loadFromDataPtr(gen, loaded_ptr);

	// if zero
	llvm::Value *condition = builder.CreateICmpEQ(
		loaded_i8,
		llvm::ConstantInt::get(context, llvm::APInt(8, 0, false)),
		"cond.");

	llvm::BasicBlock *InnerBlock =
	       	llvm::BasicBlock::Create(context, "inner", Main);

	llvm::BasicBlock *ExitBlock = llvm::BasicBlock::Create(context, "exit");

	// If zero jump to exit block
	builder.CreateCondBr(condition, ExitBlock, InnerBlock);

	// Go to innerblock
	builder.SetInsertPoint(InnerBlock);
	frame->generateCode(gen); // Emit frame

	loaded_ptr = loadDataPtr(gen);
	loaded_i8 = loadFromDataPtr(gen, loaded_ptr);

	// if zero...
	condition = builder.CreateICmpEQ(
		loaded_i8,
	       	llvm::ConstantInt::get(context, llvm::APInt(8, 0, false)));
	// ...get out to the exit block otherwise go to the beginning
	builder.CreateCondBr(condition, ExitBlock, InnerBlock);

	// Emit in exitblock
	ExitBlock->insertInto(Main);
	builder.SetInsertPoint(ExitBlock);
}

__hot void Ast::FrameAst::generateCode(CodeGenerator& gen) const
{
	for (const std::unique_ptr<ExprAst>& c : children)
		c->generateCode(gen);
}

//===-------------------------------------------------------------------------
// Command line parsing
//===-------------------------------------------------------------------------

static void printUsage()
{
	printf(
		"Usage: rbfc [options] [file.bf]\n"
		"Options:\n"
		"  -h, --help             Display this message\n"
		"  -v, --version          Display the version\n"
		"  --data-length          Set custom data length in bytes(default %ld bytes)\n"
		"  -O3                    Optimize\n"
		"  -O2                       \"\n"
		"  -O1                       \"\n"
		"  -O0                       \"\n"
		"  -c                     Do not link\n"
		"  -o                     Specify output name\n"
		"  --emit-lvm             Emit llvm IR\n"
		"  -S                     Emit asm\n",
		_DEFAULT_DATA_LENGTH
	);
}

class Options {
	OptLevel opt_level;
	bool c;
	bool S;
	long data_length;
	bool emit_llvm;
	std::optional<std::string> o;
	int argc;
	char **argv;
	std::optional<std::string> bf_src_file;
public:
	inline Options(int argc, char *argv[]) :
		opt_level(OptLevel::O2),
		c(false),
		S(false),
		data_length(_DEFAULT_DATA_LENGTH),
		emit_llvm(false),
		argc(argc),
		argv(argv) {}
	constexpr inline void setc() { c = true; }
	constexpr inline void setS() { S = true; }
	constexpr inline void setDataLength(long data_length) { this->data_length = data_length; }
	constexpr inline void setEmitLlvm() { emit_llvm = true; }
	template <typename U> // U must be a std::string
	constexpr inline void seto(U&& output) { o = std::forward<U>(output); }
	template <typename U> // U must be a std::string
	constexpr inline void setBfSrcFile(U&& filename)
		{ bf_src_file = std::forward<U>(filename); }
	constexpr inline void setOptLevel(OptLevel _opt_level)
		{ opt_level = _opt_level; }

	constexpr inline bool getc() const { return c; }
	constexpr inline bool getS() const { return S; }
	constexpr inline long getDataLength() const { return data_length; }
	constexpr inline bool getEmitLlvm() const { return emit_llvm; }
	constexpr inline const std::optional<std::string>& geto() const
		{ return o; }
	constexpr inline const std::optional<std::string>& getBfSrcFile() const
		{ return bf_src_file; }
	constexpr inline OptLevel getOptLevel() const
		{ return opt_level; }

	void parse();

	friend std::ostream& operator<<(std::ostream& os, const Options& o)
	{
		os << "c: " << o.c << std::endl <<
		      "S: " << o.S << std::endl <<
		      "data_length: " << o.data_length << std::endl <<
		      "speedupLevel: " << optLevelName(o.opt_level) << std::endl <<
		      "emit-llvm: " << o.emit_llvm;
		if (o.o)
			os << std::endl << "o: " << o.o.value();
		if (o.bf_src_file)
			os << std::endl << "src: " << o.bf_src_file.value();
		return os;
	}
};

inline void printVersion()
{
	std::cout << _VERSION << std::endl;
}

void Options::parse()
{
	struct option long_options[] = {
		{"help",        no_argument,       0, 'h'},
		{"version",     no_argument,       0, 'V'},
		{"c",           no_argument,       0, 'c'},
		{"output",      required_argument, 0, 'o'},
		{"data-length", required_argument, 0,  0},
		{"emit-llvm",   no_argument,       0,  0},
		{"S",           no_argument,       0, 'S'},
		{0, 0, 0, 0},
	};

	optind = 1;
	int option_index;
	int arg;
	while ((arg = getopt_long_only(argc,
	                               argv,
	                               "acho:O:SV",
	                               long_options,
	                               &option_index)) != EOF) {
		switch (arg) {
		case 0:
			if (strcmp(long_options[option_index].name, "emit-llvm") == 0) {
				setEmitLlvm();
			} else if (strcmp(long_options[option_index].name, "data-length") == 0) {
				long length;
				if (safe_store_atol(optarg, &length) == -1) {
					std::cerr << "The argument of --data-length `" << std::string(optarg) << "' is not a number" << std::endl;
					exit(EXIT_FAILURE);
				} else if (length < 0) {
					std::cerr << "The length of the data array must be greater or equal than zero" << std::endl;
					exit(EXIT_FAILURE);
				} else {
					setDataLength(length);
				}
			} else {
				abort(/*"Shouldn't be here"*/);
			}
			break;
		case 'O': {
			switch (*optarg) {
      case '0': setOptLevel(OptLevel::O0); break;
      case '1': setOptLevel(OptLevel::O1); break;
      case '2': setOptLevel(OptLevel::O2); break;
      case '3': setOptLevel(OptLevel::O3); break;
      case 's': setOptLevel(OptLevel::Os); break;
      case 'z': setOptLevel(OptLevel::Oz); break;
			default:
				std::cerr << "Optimization level " << optarg <<
					" not understood" << std::endl;
				exit(1);
			}
			break;
		}
		case 'h':
			printUsage();
			exit(EXIT_SUCCESS);
		case 'V':
			printVersion();
			exit(EXIT_SUCCESS);
		case 'o':
			if (!optarg) {
				std::cerr << "-o requires one argument!!!" << std::endl;
				printUsage();
				exit(EXIT_FAILURE);
			}
			seto(optarg);
			break;
		case 'S':
			setS();
			break;
		case 'c':
			setc();
			break;
		case '?':
#ifndef NDEBUG
			std::cerr << "Unrecognize option" << std::endl;
#endif
			exit(EXIT_FAILURE);
		default:
			std::cerr << "Something went wrong" << std::endl;
			exit(1);
		}
	}

	argv += optind;
	argc -= optind;

	// The problem might be that we don't want to force the user to
	// put the name of the bf file at the end of the arguments. So when
	// we find the file we start parsing againg
	if (!bf_src_file) {
		if (argc == 0) {
			std::cerr << "Missing bf source file!!!" << std::endl;
			exit(1);
		} else {
			setBfSrcFile(std::string(argv[0]));
			if (argc != 1)
				this->parse(); // Finish off parsing
		}
	} else {
		if (argc != 0) {
			std::cerr << "Too many arguments. The first of them is `"
				<< argv[0] << "'" << std::endl;
			exit(1);
		} else {
			// We have finished parsing the command line
		}
	}
	// If -o was not specified set default name which is
	// the name of the soruce gile with a suffix
	// depending on the different type of compilation
	if (!o) {
		if (getEmitLlvm()) {
			seto(getBfSrcFile().value() + std::string(".ll"));
		} else if (getS()) {
			seto(getBfSrcFile().value() + std::string(".S"));
		} else if (getc()) {
			seto(getBfSrcFile().value() + std::string(".o"));
		} else {
			seto("a.out");
		}
		// Check if default file already exists???
		// I wouldn't do that because there if of course
		// the risk of data loss but it would be really annoying
		// having to rm the file over and over before recompiling
	}
}

//===-------------------------------------------------------------------------
// Main
//===-------------------------------------------------------------------------
// Return 0 on failure
static int produce_file(llvm::Module& Module,
                        llvm::raw_fd_ostream& dest,
                        bool emit_asm,
                        OptLevel opt_level)
{
  llvm::Triple target_triple(llvm::sys::getDefaultTargetTriple());

	llvm::InitializeAllTargetInfos();
	llvm::InitializeAllTargets();
	llvm::InitializeAllTargetMCs();
	llvm::InitializeAllAsmParsers();
	llvm::InitializeAllAsmPrinters();

	std::string error;
	const llvm::Target *target =
		llvm::TargetRegistry::lookupTarget(target_triple, error);

	if (!target) {
		llvm::errs() << error;
		return 0;
	}

	std::string cpu = "generic";
	std::string features = "";

	llvm::TargetOptions target_opts;
	auto reloc_model = std::optional<llvm::Reloc::Model>(llvm::Reloc::Model::PIC_);
	llvm::TargetMachine *target_machine = target->createTargetMachine(
		target_triple, cpu, features, target_opts, reloc_model, std::nullopt,
		OptimizationLevel2CodeGenOpt(opt_level));

	Module.setDataLayout(target_machine->createDataLayout());


	llvm::legacy::PassManager pass;

	llvm::CodeGenFileType file_type =
		emit_asm ?  llvm::CodeGenFileType::AssemblyFile : llvm::CodeGenFileType::ObjectFile;
	if (target_machine->addPassesToEmitFile(pass, dest, nullptr, file_type)) {
		llvm::errs() << "target_machine can't emit a file for this type";
		return 0;
	}
	pass.run(Module);
	dest.flush();
	return 1;
}

int main(int argc, char *argv[])
{
	int retval = EXIT_SUCCESS;

	Options opts(argc, argv);
	opts.parse();

	const std::string& intput_file = opts.getBfSrcFile().value();
	const std::string& output_file = opts.geto().value();

	std::ifstream input_stream(intput_file);
	if (!input_stream) {
		std::cerr << "Unable to open file `" << intput_file << "'" << std::endl;
		return EXIT_FAILURE;
	}

	Parser parser(input_stream);
	std::unique_ptr<Ast::ExprAst> parsed = parser.parse();
	if (!parsed)
		return EXIT_FAILURE;

	CodeGenerator* cg = new CodeGenerator(opts.getOptLevel(), parsed, opts.getDataLength());

	if (opts.getEmitLlvm()) {
		FILE *open_output_file = fopen(output_file.c_str(), "w");
		if (open_output_file == NULL) {
			std::cerr << "Unable to open file `" << output_file << "'" << std::endl;
			return EXIT_FAILURE;
		}
		cg->emitLlvmIR(fileno(open_output_file));
		delete cg;
	} else if (opts.getc() || opts.getS()) {
		std::error_code ec;
		llvm::raw_fd_ostream dest(output_file, ec, llvm::sys::fs::OF_None);

		if (ec) {
			llvm::errs() << "Could not open file: " << ec.message();
			return EXIT_FAILURE;;
		}

		if (!produce_file(cg->getModule(), dest, opts.getS(), opts.getOptLevel())) {
			std::cerr << "Error while compiling" << std::endl;
			return EXIT_FAILURE;
		}
		delete cg;
	} else {
		// Use this to dump object to compile later
		std::optional<std::pair<std::string, int>> otmpfile = get_tmpfile();
		if (!otmpfile) {
			return EXIT_FAILURE;
		}
		std::pair<std::string, int> tmpfile = otmpfile.value();
		const std::string& tmpfile_name = tmpfile.first;
		int tmpfile_fd = tmpfile.second;

		llvm::raw_fd_ostream dest(tmpfile_fd, /*shouldClose=*/false);

		//std::cout << "Producing\n";
		if (!produce_file(cg->getModule(), dest, false, opts.getOptLevel())) {
			std::cerr << "Error while compiling" << std::endl;
			return EXIT_FAILURE;
		}
		//std::cout << "EndProducing\n";

		// Closing file object
		close(tmpfile_fd);

		delete cg; // Free some useless memory

		//std::cout << "Calling clang\n";
		pid_t forked_ret = fork();
		if (forked_ret == -1) {
			std::cerr << "Unable to fork process " << std::endl;
		} else if (forked_ret == 0) {
			// Child
			if (execlp(_CC, _CC, "-fPIE", tmpfile_name.c_str(), "-o", output_file.c_str(), NULL) == -1) {
				std::cerr << "error: while executing `" _CC << " -fPIE " <<
					tmpfile_name << " -o " << output_file <<
					"' failed" << std::endl;
				std::cerr << "Unable to execute linker " <<
					strerror(errno) << std::endl;
				return EXIT_FAILURE;
			}
		} else {
			// Parent
			pid_t child_pid = forked_ret;
			int wstatus;
			(void)waitpid(child_pid, &wstatus, 0);
			if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == EXIT_SUCCESS) {
				retval = EXIT_SUCCESS;
			} else {
				retval = EXIT_FAILURE;
			}
			unlink(tmpfile_name.c_str());
		}
	}
	// cg should have been deallocated in the if-elses
	return retval;
}

// vim: noai:ts=2:sw=2
