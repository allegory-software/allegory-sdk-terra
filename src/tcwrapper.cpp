/* See Copyright Notice in ../LICENSE.txt */

#include "tcwrapper.h"
#include "terra.h"
#include <assert.h>
#include <stdio.h>
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#include "tobj.h"

#include <cstdio>
#include <string>
#include <sstream>
#include <iostream>

#include "llvmheaders.h"
#include "tllvmutil.h"
#include "clang/AST/Attr.h"
#include "clang/Lex/LiteralSupport.h"
#include "tcompilerstate.h"
#include "clangpaths.h"

using namespace clang;


static void CreateTableWithName(Obj * parent, const char * name, Obj * result) {
    lua_State * L = parent->getState();
    lua_newtable(L);
    result->initFromStack(L,parent->getRefTable());
    result->push();
    parent->setfield(name);
}

// part of the setup is adapted from: http://eli.thegreenplace.net/2012/06/08/basic-source-to-source-transformation-with-clang/
// By implementing RecursiveASTVisitor, we can specify which AST nodes
// we're interested in by overriding relevant methods.
class IncludeCVisitor : public RecursiveASTVisitor<IncludeCVisitor>
{
public:
    IncludeCVisitor(Obj * res, size_t id)
        : resulttable(res),
          L(res->getState()),
          ref_table(res->getRefTable()) {
        
        //create tables for errors messages, general namespace, and the tagged namespace
        InitTable(&error_table,"errors");
        InitTable(&general,"general");
        InitTable(&tagged,"tagged");
        std::stringstream ss;
        ss << "__makeeverythinginclanglive_";
        ss << id;
        livenessfunction = ss.str();
    }
    void InitTable(Obj * tbl, const char * name) {
        CreateTableWithName(resulttable, name, tbl);
    }
    
    void InitType(const char * name, Obj * tt) {
        PushTypeField(name);
        tt->initFromStack(L,ref_table);
    }
    
    void PushTypeField(const char * name) {
        lua_getfield(L, LUA_GLOBALSINDEX, "terra");
        lua_getfield(L, -1, "types");
        lua_getfield(L, -1, name);
        lua_remove(L,-2);
        lua_remove(L,-2);
    }
    
    bool ImportError(const std::string & msg) {
        error_message = msg;
        return false;
    }
    
    bool GetFields( RecordDecl * rd, Obj * entries) {
     
        //check the fields of this struct, if any one of them is not understandable, then this struct becomes 'opaque'
        //that is, we insert the type, and link it to its llvm type, so it can be used in terra code
        //but none of its fields are exposed (since we don't understand the layout)
        bool opaque = false;
        int anonname = 0;
        for(RecordDecl::field_iterator it = rd->field_begin(), end = rd->field_end(); it != end; ++it) {
            DeclarationName declname = it->getDeclName();
            
            if(it->isBitField() || (!it->isAnonymousStructOrUnion() && !declname)) {
                opaque = true;
                continue;
            }
            std::string declstr;
            if(it->isAnonymousStructOrUnion()) {
                char buf[32];
                sprintf(buf,"_%d",anonname++);
                declstr = buf;
            } else {
                declstr = declname.getAsString();
            }
            QualType FT = it->getType();
            Obj fobj;
            if(!GetType(FT,&fobj)) {
                opaque = true;
                continue;
            }
            lua_newtable(L);
            fobj.push();
            lua_setfield(L,-2,"type");
            lua_pushstring(L,declstr.c_str());
            lua_setfield(L,-2,"field");
            entries->addentry();
        }
        return !opaque;

    }
    void RegisterRecordType(QualType T, std::string * definingfunction, size_t * argpos) {
        *definingfunction = livenessfunction;
        *argpos = outputtypes.size();
        outputtypes.push_back(Context->getPointerType(T));
        assert(outputtypes.size() < 65536 && "fixme: clang limits number of arguments to 65536");
    }
    bool GetRecordTypeFromDecl(RecordDecl * rd, Obj * tt) {
        if(rd->isStruct() || rd->isUnion()) {
            std::string name = rd->getName();
            Obj * thenamespace = &tagged;
            if(name == "") {
                TypedefNameDecl * decl = rd->getTypedefNameForAnonDecl();
                if(decl) {
                    thenamespace = &general;
                    name = decl->getName();
                }
            }
            //if name == "" then we have an anonymous struct
            if(!thenamespace->obj(name.c_str(),tt)) {
                PushTypeField("getorcreatecstruct");
                lua_pushstring(L, name.c_str());
                lua_pushboolean(L, thenamespace == &tagged);
                lua_call(L,2,1);
                tt->initFromStack(L,ref_table);
                if(!tt->boolean("llvm_definingfunction")) {
                    std::string definingfunction;
                    size_t argpos;
                    RegisterRecordType(Context->getRecordType(rd), &definingfunction, &argpos);
                    lua_pushstring(L,definingfunction.c_str());
                    tt->setfield("llvm_definingfunction");
                    lua_pushinteger(L,argpos);
                    tt->setfield("llvm_argumentposition");
                }
                if(name != "") { //do not remember a name for an anonymous struct
                    tt->push();
                    thenamespace->setfield(name.c_str()); //register the type
                }
            }
            
            if(tt->boolean("undefined") && rd->getDefinition() != NULL) {
                tt->clearfield("undefined");
                RecordDecl * defn = rd->getDefinition();
                Obj entries;
                tt->newlist(&entries);
                if(GetFields(defn, &entries)) {
                    if(!defn->isUnion()) {
                        //structtype.entries = {entry1, entry2, ... }
                        entries.push();
                        tt->setfield("entries");
                    } else {
                        //add as a union:
                        //structtype.entries = { {entry1,entry2,...} }
                        Obj allentries;
                        tt->obj("entries",&allentries);
                        entries.push();
                        allentries.addentry();
                    }
                    tt->pushfield("complete");
                    tt->push();
                    lua_call(L,1,0);
                }
            }
            
            return true;
        } else {
            return ImportError("non-struct record types are not supported");
        }
    }

    bool GetType(QualType T, Obj * tt) {
        
        T = Context->getCanonicalType(T);
        const Type *Ty = T.getTypePtr();
        
        switch (Ty->getTypeClass()) {
          case Type::Record: {
            const RecordType *RT = dyn_cast<RecordType>(Ty);
            RecordDecl * rd = RT->getDecl();
            return GetRecordTypeFromDecl(rd, tt);
          }  break; //TODO
          case Type::Builtin:
            switch (cast<BuiltinType>(Ty)->getKind()) {
            case BuiltinType::Void:
              InitType("opaque",tt);
                return true;
            case BuiltinType::Bool:
                InitType("bool",tt);
                return true;
            case BuiltinType::Char_S:
            case BuiltinType::Char_U:
            case BuiltinType::SChar:
            case BuiltinType::UChar:
            case BuiltinType::Short:
            case BuiltinType::UShort:
            case BuiltinType::Int:
            case BuiltinType::UInt:
            case BuiltinType::Long:
            case BuiltinType::ULong:
            case BuiltinType::LongLong:
            case BuiltinType::ULongLong:
            case BuiltinType::WChar_S:
            case BuiltinType::WChar_U:
            case BuiltinType::Char16:
            case BuiltinType::Char32: {
                std::stringstream ss;
                if (Ty->isUnsignedIntegerType())
                    ss << "u";
                ss << "int";
                int sz = Context->getTypeSize(T);
                ss << sz;
                InitType(ss.str().c_str(),tt);
                return true;
            }
            case BuiltinType::Half:
                break;
            case BuiltinType::Float:
                InitType("float",tt);
                return true;
            case BuiltinType::Double:
                InitType("double",tt);
                return true;
            case BuiltinType::LongDouble:
            case BuiltinType::NullPtr:
            case BuiltinType::UInt128:
            default:
                break;
            }
          case Type::Complex:
          case Type::LValueReference:
          case Type::RValueReference:
            break;
          case Type::Pointer: {
            const PointerType *PTy = cast<PointerType>(Ty);
            QualType ETy = PTy->getPointeeType();
            Obj t2;
            if(!GetType(ETy,&t2)) {
                return false;
            }
            PushTypeField("pointer");
            t2.push();
            lua_call(L,1,1);
            tt->initFromStack(L, ref_table);
            return true;
          }
          
          case Type::VariableArray:
          case Type::IncompleteArray:
            break;
          case Type::ConstantArray: {
            Obj at;
            const ConstantArrayType *ATy = cast<ConstantArrayType>(Ty);
            int sz = ATy->getSize().getZExtValue();
            if(GetType(ATy->getElementType(),&at)) {
                PushTypeField("array");
                at.push();
                lua_pushinteger(L, sz);
                lua_call(L,2,1);
                tt->initFromStack(L,ref_table);
                return true;
            } else {
                return false;
            }
          } break;
          case Type::ExtVector:
          case Type::Vector: {
                //printf("making a vector!\n");
                const VectorType *VT = cast<VectorType>(T);
                Obj at;
                if(GetType(VT->getElementType(),&at)) {
                    int n = VT->getNumElements();
                    PushTypeField("vector");
                    at.push();
                    lua_pushinteger(L,n);
                    lua_call(L,2,1);
                    tt->initFromStack(L, ref_table);
                    return true;
                } else {
                    return false;
                }
          } break;
          case Type::FunctionNoProto:
                break;
          case Type::FunctionProto: {
                const FunctionProtoType *FT = cast<FunctionProtoType>(Ty);
                //call functype... getNumArgs();
                if(FT && GetFuncType(FT,tt))
                    return true;
                else
                    return false;
                break;
          }
          case Type::ObjCObject:
          case Type::ObjCInterface:
          case Type::ObjCObjectPointer:
          case Type::Enum:
            InitType("uint32",tt);
            return true;
          case Type::BlockPointer:
          case Type::MemberPointer:
          case Type::Atomic:
          default:
            break;
        }
        std::stringstream ss;
        ss << "type not understood: " << T.getAsString().c_str() << " " << Ty->getTypeClass();
        return ImportError(ss.str().c_str());
    }
    void SetErrorReport(const char * field) {
        lua_pushstring(L,error_message.c_str());
        error_table.setfield(field);
    }
    CStyleCastExpr* CreateCast(QualType Ty, CastKind Kind, Expr *E) {
      TypeSourceInfo *TInfo = Context->getTrivialTypeSourceInfo(Ty, SourceLocation());
      return CStyleCastExpr::Create(*Context, Ty, VK_RValue, Kind, E, 0, TInfo,
                                    SourceLocation(), SourceLocation());
    }
    IntegerLiteral * LiteralZero() {
        unsigned IntSize = static_cast<unsigned>(Context->getTypeSize(Context->IntTy));
        return IntegerLiteral::Create(*Context, llvm::APInt(IntSize, 0), Context->IntTy, SourceLocation());
    }
    DeclRefExpr * GetFunctionReference(FunctionDecl * df) {
        DeclRefExpr *DR = DeclRefExpr::Create(*Context, NestedNameSpecifierLoc(),SourceLocation(),df, false, SourceLocation(),
                          df->getType(),
                          VK_LValue);
        return DR;
    }
    void KeepFunctionLive(FunctionDecl * df) {
        Expr * castexp = CreateCast(Context->VoidTy, clang::CK_ToVoid, GetFunctionReference(df));
        outputstmts.push_back(castexp);
    }
    bool VisitTypedefDecl(TypedefDecl * TD) {
        if(TD == TD->getCanonicalDecl() && TD->getDeclContext()->getDeclKind() == Decl::TranslationUnit) {
            llvm::StringRef name = TD->getName();
            QualType QT = Context->getCanonicalType(TD->getUnderlyingType());
            Obj typ;
            if(GetType(QT,&typ)) {
                typ.push();
                general.setfield(name.str().c_str());
            } else {
                SetErrorReport(name.str().c_str());
            }
        }
        return true;
    }
    bool TraverseRecordDecl(RecordDecl * rd) {
        if(rd->getDeclContext()->getDeclKind() == Decl::TranslationUnit) {
            Obj type;
            GetRecordTypeFromDecl(rd, &type);
        }
        return true;
    }
    bool VisitEnumConstantDecl(EnumConstantDecl * E) {
        int64_t v = E->getInitVal().getSExtValue();
        llvm::StringRef name = E->getName();
        lua_pushnumber(L,(double)v); //I _think_ enums by spec must fit in an int, so they will fit in a double
        general.setfield(name.str().c_str());
        return true;
    }
    bool GetFuncType(const FunctionType * f, Obj * typ) {
        Obj returntype,parameters;
        resulttable->newlist(&parameters);
        
        bool valid = true; //decisions about whether this function can be exported or not are delayed until we have seen all the potential problems
    #if LLVM_VERSION <= 34
        QualType RT = f->getResultType();
    #else
        QualType RT = f->getReturnType();
    #endif
        if(RT->isVoidType()) {
            PushTypeField("unit");
            returntype.initFromStack(L, ref_table);
        } else {
            if(!GetType(RT,&returntype))
                valid = false;
        }
       
        
        const FunctionProtoType * proto = f->getAs<FunctionProtoType>();
        //proto is null if the function was declared without an argument list (e.g. void foo() and not void foo(void))
        //we don't support old-style C parameter lists, we just treat them as empty
        if(proto) {
        #if LLVM_VERSION >= 35
            for(size_t i = 0; i < proto->getNumParams(); i++) {
                QualType PT = proto->getParamType(i);
        #else
            for(size_t i = 0; i < proto->getNumArgs(); i++) {
                QualType PT = proto->getArgType(i);
        #endif
                Obj pt;
                if(!GetType(PT,&pt)) {
                    valid = false; //keep going with attempting to parse type to make sure we see all the reasons why we cannot support this function
                } else if(valid) {
                    pt.push();
                    parameters.addentry();
                }
            }
        }
        
        if(valid) {
            PushTypeField("functype");
            parameters.push();
            returntype.push();
            lua_pushboolean(L, proto ? proto->isVariadic() : false);
            lua_call(L, 3, 1);
            typ->initFromStack(L,ref_table);
        }
        
        return valid;
    }
    bool TraverseFunctionDecl(FunctionDecl *f) {
         // Function name
        DeclarationName DeclName = f->getNameInfo().getName();
        std::string FuncName = DeclName.getAsString();
        const FunctionType * fntyp = f->getType()->getAs<FunctionType>();
        
        if(!fntyp)
            return true;
        
        if(f->getStorageClass() == clang::SC_Static) {
            ImportError("cannot import static functions.");
            SetErrorReport(FuncName.c_str());
            return true;
        }
        
        Obj typ;
        if(!GetFuncType(fntyp,&typ)) {
            SetErrorReport(FuncName.c_str());
            return true;
        }
        std::string InternalName = FuncName;
        AsmLabelAttr * asmlabel = f->getAttr<AsmLabelAttr>();
        if(asmlabel) {
            InternalName = asmlabel->getLabel();
            #ifndef __linux__
                //In OSX and Windows LLVM mangles assembler labels by adding a '\01' prefix
                InternalName.insert(InternalName.begin(), '\01');
            #endif
        }
        CreateFunction(FuncName,InternalName,&typ);
        
        KeepFunctionLive(f);//make sure this function is live in codegen by creating a dummy reference to it (void) is to suppress unused warnings
        
        return true;
    }
    void CreateFunction(const std::string & name, const std::string & internalname, Obj * typ) {
        if(!general.hasfield(name.c_str())) {
            lua_getfield(L, LUA_GLOBALSINDEX, "terra");
            lua_getfield(L, -1, "externfunction");
            lua_remove(L,-2); //terra table
            lua_pushstring(L, internalname.c_str());
            typ->push();
            lua_call(L, 2, 1);
            general.setfield(name.c_str());
        }
    }
    void SetContext(ASTContext * ctx) {
        Context = ctx;
    }
    FunctionDecl * GetLivenessFunction() {
        IdentifierInfo & II = Context->Idents.get(livenessfunction);
        DeclarationName N = Context->DeclarationNames.getIdentifier(&II);
        #if LLVM_VERSION >= 33
        QualType T = Context->getFunctionType(Context->VoidTy, outputtypes, FunctionProtoType::ExtProtoInfo());
        #else
        QualType T = Context->getFunctionType(Context->VoidTy, &outputtypes[0],outputtypes.size(), FunctionProtoType::ExtProtoInfo());
        #endif
        FunctionDecl * F = FunctionDecl::Create(*Context, Context->getTranslationUnitDecl(), SourceLocation(), SourceLocation(), N,T, 0, SC_Extern);
        
        std::vector<ParmVarDecl *> params;
        for(size_t i = 0; i < outputtypes.size(); i++) {
            params.push_back(ParmVarDecl::Create(*Context, F, SourceLocation(), SourceLocation(), 0, outputtypes[i], /*TInfo=*/0, SC_None,
            #if LLVM_VERSION <= 32
            SC_None,
            #endif
            0));
        }
        F->setParams(params);
        #if LLVM_VERSION >= 33
        CompoundStmt * stmts = new (*Context) CompoundStmt(*Context, outputstmts, SourceLocation(), SourceLocation());
        #else
        CompoundStmt * stmts = new (*Context) CompoundStmt(*Context, &outputstmts[0], outputstmts.size(), SourceLocation(), SourceLocation());
        #endif
        F->setBody(stmts);
        return F;
    }
  private:
    std::vector<Stmt*> outputstmts;
    std::vector<QualType> outputtypes;
    Obj * resulttable; //holds table returned to lua includes "functions", "types", and "errors"
    lua_State * L;
    int ref_table;
    ASTContext * Context;
    Obj error_table; //name -> related error message
    Obj general; //name -> function or type in the general namespace
    Obj tagged; //name -> type in the tagged namespace (e.g. struct Foo)
    std::string error_message;
    std::string livenessfunction;
};

class CodeGenProxy : public ASTConsumer {
public:
  CodeGenProxy(CodeGenerator * CG_, Obj * result, size_t importid)
  : CG(CG_), Visitor(result,importid) {}
  CodeGenerator * CG;
  IncludeCVisitor Visitor;
  virtual ~CodeGenProxy() {}
  virtual void Initialize(ASTContext &Context) {
    Visitor.SetContext(&Context);
    CG->Initialize(Context);
  }
  virtual bool HandleTopLevelDecl(DeclGroupRef D) {
    for (DeclGroupRef::iterator b = D.begin(), e = D.end();
         b != e; ++b)
            Visitor.TraverseDecl(*b);
    return CG->HandleTopLevelDecl(D);
  }
  virtual void HandleInterestingDecl(DeclGroupRef D) { CG->HandleInterestingDecl(D); }
  virtual void HandleTranslationUnit(ASTContext &Ctx) {
    Decl * Decl = Visitor.GetLivenessFunction();
    DeclGroupRef R = DeclGroupRef::Create(Ctx, &Decl, 1);
    CG->HandleTopLevelDecl(R);
    CG->HandleTranslationUnit(Ctx);
  }
  virtual void HandleTagDeclDefinition(TagDecl *D) { CG->HandleTagDeclDefinition(D); }
  virtual void HandleCXXImplicitFunctionInstantiation(FunctionDecl *D) { CG->HandleCXXImplicitFunctionInstantiation(D); }
  virtual void HandleTopLevelDeclInObjCContainer(DeclGroupRef D) { CG->HandleTopLevelDeclInObjCContainer(D); }
  virtual void CompleteTentativeDefinition(VarDecl *D) { CG->CompleteTentativeDefinition(D); }
  virtual void HandleCXXStaticMemberVarInstantiation(VarDecl *D) { CG->HandleCXXStaticMemberVarInstantiation(D); }
  virtual void HandleVTable(CXXRecordDecl *RD, bool DefinitionRequired) { CG->HandleVTable(RD, DefinitionRequired); }
  virtual ASTMutationListener *GetASTMutationListener() { return CG->GetASTMutationListener(); }
  virtual ASTDeserializationListener *GetASTDeserializationListener() { return CG->GetASTDeserializationListener(); }
  virtual void PrintStats() { CG->PrintStats(); }

#if LLVM_VERSION == 32
  virtual void HandleImplicitImportDecl(ImportDecl *D) { CG->HandleImplicitImportDecl(D); }
  virtual PPMutationListener *GetPPMutationListener() { return CG->GetPPMutationListener(); }
#elif LLVM_VERSION >= 33
  virtual void HandleImplicitImportDecl(ImportDecl *D) { CG->HandleImplicitImportDecl(D); }
  virtual bool shouldSkipFunctionBody(Decl *D) { return CG->shouldSkipFunctionBody(D); }
#endif

};

static void initializeclang(terra_State * T, llvm::MemoryBuffer * membuffer, const char ** argbegin, const char ** argend, CompilerInstance * TheCompInst) {
    // CompilerInstance will hold the instance of the Clang compiler for us,
    // managing the various objects needed to run the compiler.
    #if LLVM_VERSION <= 32
    TheCompInst->createDiagnostics(0, 0);
    #else
    TheCompInst->createDiagnostics();
    #endif
    
    CompilerInvocation::CreateFromArgs(TheCompInst->getInvocation(), argbegin, argend, TheCompInst->getDiagnostics());
    //need to recreate the diagnostics engine so that it actually listens to warning flags like -Wno-deprecated
    //this cannot go before CreateFromArgs
    #if LLVM_VERSION <= 32
    TheCompInst->createDiagnostics(argbegin - argend, argbegin);
    TargetOptions & to = TheCompInst->getTargetOpts();
    #elif LLVM_VERSION <= 34
    TheCompInst->createDiagnostics();
    TargetOptions * to = &TheCompInst->getTargetOpts();
    #else
    TheCompInst->createDiagnostics();
    std::shared_ptr<TargetOptions> to(new TargetOptions(TheCompInst->getTargetOpts()));
    #endif
    
    TargetInfo *TI = TargetInfo::CreateTargetInfo(TheCompInst->getDiagnostics(), to);
    TheCompInst->setTarget(TI);
    
    TheCompInst->createFileManager();
    FileManager &FileMgr = TheCompInst->getFileManager();
    TheCompInst->createSourceManager(FileMgr);
    SourceManager &SourceMgr = TheCompInst->getSourceManager();
    TheCompInst->createPreprocessor(
    #if LLVM_VERSION >= 35
    TU_Complete
    #endif
    );
    TheCompInst->createASTContext();

    // Set the main file handled by the source manager to the input file.
#if LLVM_VERSION <= 34
    SourceMgr.createMainFileIDForMemBuffer(membuffer);
#else   
    SourceMgr.setMainFileID(SourceMgr.createFileID(membuffer));
#endif
    TheCompInst->getDiagnosticClient().BeginSourceFile(TheCompInst->getLangOpts(),&TheCompInst->getPreprocessor());
    Preprocessor &PP = TheCompInst->getPreprocessor();
    PP.getBuiltinInfo().InitializeBuiltins(PP.getIdentifierTable(),
                                           PP.getLangOpts());
    
}
#if LLVM_VERSION >= 33
static void AddMacro(terra_State * T, Preprocessor & PP, const IdentifierInfo * II, MacroDirective * MD, Obj * table) {

    if(!II->hasMacroDefinition())
        return;
    MacroInfo * MI = MD->getMacroInfo();
    if(MI->isFunctionLike())
        return;
    bool negate = false;
    const Token * Tok;
    if(MI->getNumTokens() == 2 && MI->getReplacementToken(0).is(clang::tok::minus)) {
        negate = true;
        Tok = &MI->getReplacementToken(1);
    } else if(MI->getNumTokens() == 1) {
        Tok = &MI->getReplacementToken(0);
    } else {
        return;
    }
    
    if(Tok->isNot(clang::tok::numeric_constant))
        return;
    
    SmallString<64> IntegerBuffer;
    bool NumberInvalid = false;
    StringRef Spelling = PP.getSpelling(*Tok, IntegerBuffer, &NumberInvalid);
    NumericLiteralParser Literal(Spelling, Tok->getLocation(), PP);
    if(Literal.hadError)
        return;
    double V;
    if(Literal.isFloatingLiteral()) {
        llvm::APFloat Result(0.0);
        Literal.GetFloatValue(Result);
        V = Result.convertToDouble();
    } else {
        llvm::APInt Result(64,0);
        Literal.GetIntegerValue(Result);
        int64_t i = Result.getSExtValue();
        if((int64_t)(double)i != i)
            return; //right now we ignore things that are not representable in Lua's number type
                    //eventually we should use LuaJITs ctype support to hold the larger numbers
        V = i;
    }
    if(negate)
        V = -V;
    lua_pushnumber(T->L,V);
    table->setfield(II->getName().str().c_str());
}
#endif
static int dofile(terra_State * T, const char * code, const char ** argbegin, const char ** argend, Obj * result) {
    // CompilerInstance will hold the instance of the Clang compiler for us,
    // managing the various objects needed to run the compiler.
    CompilerInstance TheCompInst;
    llvm::MemoryBuffer * membuffer = llvm::MemoryBuffer::getMemBuffer(code, "<buffer>");
    initializeclang(T, membuffer, argbegin, argend, &TheCompInst);
    
    #if LLVM_VERSION <= 32
    CodeGenerator * codegen = CreateLLVMCodeGen(TheCompInst.getDiagnostics(), "mymodule", TheCompInst.getCodeGenOpts(), *T->C->ctx );
    #else
    CodeGenerator * codegen = CreateLLVMCodeGen(TheCompInst.getDiagnostics(), "mymodule", TheCompInst.getCodeGenOpts(), TheCompInst.getTargetOpts(), *T->C->ctx );
    #endif
    
    CodeGenProxy proxy(codegen,result,T->C->next_unused_id++);
    ParseAST(TheCompInst.getPreprocessor(),
            &proxy,
            TheCompInst.getASTContext());
    
    Obj macros;
    CreateTableWithName(result, "macros", &macros);
    #if LLVM_VERSION >= 33
    Preprocessor & PP = TheCompInst.getPreprocessor();
    for(Preprocessor::macro_iterator it = PP.macro_begin(false),end = PP.macro_end(false); it != end; ++it) {
        const IdentifierInfo * II = it->first;
        MacroDirective * MD = it->second;
        AddMacro(T,PP,II,MD,&macros);
    }
    #endif
    
    llvm::Module * mod = codegen->ReleaseModule();
    
    if(mod) {
        std::string err;
        VERBOSE_ONLY(T) {
            mod->dump();
        }
        if(llvmutil_linkmodule(T->C->m, mod, T->C->tm,&T->C->cwrapperpm, &err)) {
            delete codegen;
            terra_reporterror(T,"llvm: %s\n",err.c_str());
        }
    } else {
        delete codegen;
        terra_reporterror(T,"compilation of included c code failed\n");
    }
    
    delete codegen;
    
    return 0;
}

int include_c(lua_State * L) {
    terra_State * T = terra_getstate(L, 1);
    const char * code = luaL_checkstring(L, -2);
    int N = lua_objlen(L, -1);
    std::vector<const char *> args;

#if defined(_WIN32) && !defined(__MINGW32__)
    args.push_back("-fms-extensions");
    args.push_back("-fms-compatibility");
#define __stringify(x) #x
#define __indirect(x) __stringify(x)
    args.push_back("-fms-compatibility-version=" __indirect(_MSC_VER));
	args.push_back("-Wno-ignored-attributes");
#endif
    
    for(int i = 0; i < N; i++) {
        lua_rawgeti(L, -1, i+1);
        args.push_back(luaL_checkstring(L,-1));
        lua_pop(L,1);
    }
    
    const char ** cpaths = clang_paths;
    while(*cpaths) {
        args.push_back(*cpaths);
        cpaths++;
    }
    
    lua_newtable(L); //return a table of loaded functions
    int ref_table = lobj_newreftable(L);
    {
        Obj result;
        lua_pushvalue(L, -2);
        result.initFromStack(L, ref_table);
        
        dofile(T,code,&args[0],&args[args.size()],&result);
    }
    
    lobj_removereftable(L, ref_table);
    return 1;
}

void terra_cwrapperinit(terra_State * T) {
    lua_getfield(T->L,LUA_GLOBALSINDEX,"terra");
    
    lua_pushlightuserdata(T->L,(void*)T);
    lua_pushcclosure(T->L,include_c,1);
    lua_setfield(T->L,-2,"registercfile");
    
    lua_pop(T->L,-1); //terra object
}
