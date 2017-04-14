#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Signals.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Main.h"
#include "llvm/TableGen/Record.h"

using namespace llvm;

enum class ActionType {
  None,
  GenImplementation,
  GenInterface,
};

enum class Category {
  Unknown,
  Decl,
  Stmt,
  Expr,
  Type,
  Pattern,
  SyntaxFactory,
  SyntaxRewriter
};

enum class TargetLanguage {
  CPlusPlus,
};

RecordKeeper *AllRecords;
SmallPtrSet<const Record *, 8> SyntaxCategories;

namespace options {
static cl::opt<ActionType> Action(
  cl::desc("Action to perform: "),
  cl::init(ActionType::None),
  cl::values(
    clEnumValN(ActionType::GenImplementation, "implementation",
      "Generate the implementation for the given syntax category"),
    clEnumValN(ActionType::GenInterface, "interface",
      "Generate the interface for the given syntax category")
    )
);
static cl::opt<TargetLanguage> Language(
  "language",
  cl::desc("Target language to emit: "),
  cl::init(TargetLanguage::CPlusPlus),
  cl::values(
    clEnumValN(TargetLanguage::CPlusPlus, "c++",
      "Generate the interface or implementation in C++")
  )
);
static cl::opt<std::string> Category(
  "category",
  cl::desc("Category to emit"));

} // end options namespace

#pragma mark - Helpers

/// Returns true if the record type is a subclass of Syntax.
/// Used to filter out values in a record that tablegen automatically inserts, like NAME,
/// or auxiliary fields that we have added, like IsRequired.
static bool is(const RecTy *Ty, StringRef TypeName) {
  if (auto RecordTy = dyn_cast<RecordRecTy>(Ty)) {
    if (RecordTy->getAsString() == TypeName || RecordTy->getRecord()->isSubClassOf(AllRecords->getClass(TypeName))) {
      return RecordTy;
    }
  }
  return nullptr;
}

static bool isToken(const RecTy *Ty) {
  return is(Ty, "Token");
}

static bool isIdentifier(const RecTy *Ty) {
  return is(Ty, "Identifier");
}

static bool isLayout(const RecTy *Ty) {
  return is(Ty, "Layout");
}

static const Record *getLayoutNodeRecord(const Record *Rec) {
  auto NodeField = Rec->getValue("Node");
  auto NodeValue = cast<DefInit>(NodeField->getValue());
  return NodeValue->getDef();
}

static const Record *getLayoutNodeRecord(const RecordVal Child) {
  assert(isLayout(Child.getType()));
  auto RHS = cast<DefInit>(Child.getValue());
  auto NodeDef = RHS->getDef();
  return getLayoutNodeRecord(NodeDef);
}

static llvm::Optional<const Record *>
getSyntaxCategory(const Record *Def) {
  for (const auto Super : Def->getSuperClasses()) {
    if (SyntaxCategories.count(Super.first)) {
      return Super.first;
    }
  }
  return None;
}

static const RecordRecTy *getLayoutNodeType(const RecordVal Child) {
  assert(isLayout(Child.getType()));
  auto RHS = cast<DefInit>(Child.getValue());
  auto NodeDef = RHS->getDef();
  auto NodeField = NodeDef->getValue("Node");
  auto NodeValue = cast<DefInit>(NodeField->getValue());
  auto NodeType = NodeValue->getType();
  return cast<RecordRecTy>(NodeType);
}

static StringRef getTokenSpelling(const RecordVal Value) {
  assert(isLayout(Value.getType()));
  auto TokenRec = getLayoutNodeRecord(Value);
  for (const auto Field : TokenRec->getValues()) {
    if (Field.getName() == "Spelling") {
      return Field.getValue()->getAsUnquotedString();
    }
  }
  return StringRef();
}

static std::string getMissingSyntaxKind(const RecordVal Child) {
  auto Node = getLayoutNodeRecord(Child);
  auto Category = getSyntaxCategory(Node).getValue();
  return llvm::StringSwitch<std::string>(Category->getName())
    .Case("Decl", "MissingDecl")
    .Case("Expr", "MissingExpr")
    .Case("Stmt", "MissingStmt")
    .Case("Type", "MissingType")
    .Case("Pattern", "MissingPattern")
    .Case("SyntaxCollection", "MissingSyntaxCollection");
}

static Category getCategory() {
  return llvm::StringSwitch<Category>(options::Category)
  .Case("Decl", Category::Decl)
  .Case("Expr", Category::Expr)
  .Case("Stmt", Category::Stmt)
  .Case("Type", Category::Type)
  .Case("Pattern", Category::Pattern)
  .Case("SyntaxFactory", Category::SyntaxFactory)
  .Case("SyntaxRewriter", Category::SyntaxRewriter)
  .Default(Category::Unknown);
}

static std::vector<RecordVal> getChildrenOf(const Record *Node) {
  std::vector<RecordVal> Children;

  for (const auto Child : Node->getValues()) {
    if (!isLayout(Child.getType())) {
      continue;
    }
    Children.push_back(Child);
  }

  return Children;
}

static void printTokenAssertion(std::string VariableName,
                                const RecordVal Value,
                                raw_ostream &OS) {
  assert(isLayout(Value.getType()));
  auto Ty = getLayoutNodeType(Value);
  assert(isToken(Ty));
  auto TokenRec = getLayoutNodeRecord(Value);
  auto Kind = TokenRec->getValueAsString("Kind");
  if (isIdentifier(Ty)) {
    OS << "assert(" << VariableName << "->getTokenKind() == " << Kind << ");\n";
  } else {
    OS << "  syntax_assert_token_is(" << VariableName << ", " << Kind << ", \"" << getTokenSpelling(Value) << "\");\n";
  }
}

#pragma mark - Syntax

static bool printSyntaxInterface(const Record *Node, raw_ostream &OS) {
  auto ClassName = std::string(Node->getName()) + "Syntax";
  auto SuperclassName = Node->getSuperClasses().back().first->getName() + "Syntax";
  auto DataClassName = ClassName + "Data";

  OS << "class " << ClassName << " final : public Syntax {\n"
  "  friend struct SyntaxFactory;\n"
  "  friend class " << DataClassName << ";\n"
  "  friend class SyntaxData;\n"
  "\n"
  "  using DataType = " << DataClassName << ";\n"
  "\n"
  "  enum class Cursor : CursorIndex {\n";
  for (const auto Child : getChildrenOf(Node)) {
    OS << "    " << Child.getName() << ",\n";
  }
  OS << "  };\n";

  OS <<
  "  " << ClassName << "(RC<SyntaxData> Root, const " << DataClassName << "*Data)\n"
  "    : " << SuperclassName << "(Root, Data) {}\n"
  "public:\n";
  for (const auto Child : getChildrenOf(Node)) {
    auto ChildType = cast<RecordRecTy>(Child.getType());
    auto ChildTypeName = ChildType->getAsString() + "Syntax";
    auto OptionalChildTypeName = "llvm::Optional<" + ChildTypeName + ">";
    if (ChildType->getRecord() == AllRecords->getClass("Token")) {
      ChildTypeName = "RC<" + ChildTypeName + ">";
    }

    OS << "  " << OptionalChildTypeName << " get" << Child.getName() << "() const;\n";
    OS << "  " << ClassName << " with" << Child.getName() << "(" << ChildTypeName << " New" << Child.getName() << ") const;\n\n";
  }
  OS <<
  "  static bool classof(const Syntax *S) {\n"
  "    return S->getKind() == SyntaxKind::" << ClassName << ";\n"
  "  }\n"
  "};\n\n";

  return false;
}

static bool printSyntaxDataInterface(const Record *Node,
                                     raw_ostream &OS) {
  auto Kind = Node->getName();
  auto ClassName = std::string(Kind) + "Syntax";
  auto DataClassName = ClassName + "Data";

  OS << "class " << DataClassName << " final : public SyntaxData {\n"
  "  friend class SyntaxData;\n"
  "  friend struct SyntaxFactory;\n"
  "\n" <<
  "  " << DataClassName << "(RC<RawSyntax> Raw, const SyntaxData *Parent = nullptr, CursorIndex IndexInParent = 0);\n"
  "\n"
  "  static RC<" << DataClassName << "> make(RC<RawSyntax> Raw, const SyntaxData *Parent = nullptr, CursorIndex IndexInParent = 0);\n"
  "  static RC<" << DataClassName << "> makeBlank();\n"
  "\n"
  "public:\n"
  "  static bool classof(const SyntaxData *SD) {\n"
  "    return S->getKind() == SyntaxKind::" << Kind << ";\n"
  "  }\n"
  "};\n\n";
  return false;
}

static bool printSyntaxInterfaces(raw_ostream &OS) {
  auto Nodes = AllRecords->getAllDerivedDefinitions(options::Category);
  auto Any = std::string { "Any" } + options::Category;
  for (const auto *Node : Nodes) {
    if (Node->getName() == Any) {
      continue;
    }
    printSyntaxInterface(Node, OS);
    printSyntaxDataInterface(Node, OS);
  }

  return false;
}

static bool printSyntaxImplementation(const Record *Node,
                                      raw_ostream &OS) {
  auto Kind = Node->getName();
  auto ClassName = std::string(Kind);
  auto DataClassName = ClassName + "Data";

  for (auto Child : getChildrenOf(Node)) {
    auto ChildName = Child.getName();
    auto ChildType = getLayoutNodeType(Child);
    auto ChildTypeName = ChildType->getAsString();
    auto OptionalChildTypeName = "llvm::Optional<" + ChildTypeName + ">";

    // Getter
    OS << OptionalChildTypeName << "\n" <<
    ClassName << "::get" << ChildName << "() const {\n"
    "  auto RawChild = getRaw()->getChild(Cursor::" << ChildName << ";\n"
    "  if (RawChild->isMissing()) {\n"
    "    return llvm::None;\n"
    "  }\n"
    "  auto *MyData = getUnsafeData<" << ClassName << ">();\n"
    "  auto &ChildPtr = *reinterpret_cast<std::atomic<uintptr_t>*>(\n"
    "    &MyData->Cached" << ChildName << ");\n"
    "  SyntaxData::realizeSyntaxNode<" << ChildTypeName << ">(ChildPtr, RawChild, MyData, cursorIndex(Cursor::" << ChildName << "));\n"
    "  return " << ClassName << "{ Root, MyData->Cached" << ChildName << ".get() };\n"
    "}\n\n";

    // Setter
    auto NewChildArg = "New" + ChildName;
    OS << ClassName << "\n" << ClassName << "::with" << ChildName << "(" << ChildTypeName << " " << NewChildArg << ") const {\n";
    if (isToken(ChildType)) {
      printTokenAssertion(NewChildArg.str(), Child, OS);
      OS << "  return Data->replaceChild<" << ClassName << ">(" << NewChildArg << ", " << "Cursor::" << ChildName << ");\n";
    } else {
      OS << "  return Data->replaceChild<" << ClassName << ">(" << NewChildArg << "->getRaw(), " << "Cursor::" << ChildName << ");\n";
    }
    OS << "}\n\n";
  }

  return false;
}

static bool printSyntaxDataImplementation(const Record *Node, raw_ostream &OS) {
  auto Kind = Node->getName();
  auto ClassName = std::string(Kind);
  auto SuperclassName = Node->getSuperClasses().back().first->getName();
  auto DataClassName = ClassName + "Data";
  auto DataSuperclassName = SuperclassName + "Data";

  // Constructor
  OS << DataClassName << "::" << DataClassName << "(RC<RawSyntax> Raw, const SyntaxData *Data, const CursorIndex IndexInParent)\n"
  "  : " << DataSuperclassName << "(Raw, Data, IndexInParent) {\n"
  "  assert(Raw->getKind() == SyntaxKind::" << Kind << ");\n"
  "  assert(Raw->Layout.size() == " << getChildrenOf(Node).size() << ");\n";
  for (const auto Child : getChildrenOf(Node)) {
    auto ChildName = Child.getName();
    auto ChildType = getLayoutNodeType(Child);
    auto ChildTypeName = ChildType->getAsString();
    auto ChildVariable = "Raw->getChild(Cursor::" + ChildName + ")";
    if (isToken(ChildType)) {
      printTokenAssertion(ChildVariable.str(), Child, OS);
    } else {
      OS << "  assert(" << ChildVariable << "->getKind() == SyntaxKind::" << ChildTypeName << ");\n";
    }
  }
  OS << "}\n\n";

  // make
  OS << "RC<" << DataClassName << ">\n" <<
  DataClassName << "::make(RC<RawSyntax> Raw, const SyntaxData *Parent, const CursorIndex IndexInParent) {\n"
  "  return RC<" << DataClassName << "> {\n"
  "    new " << DataClassName << " { Raw, Parent, IndexInParent }\n"
  "  };\n"
  "}\n\n";

  // makeBlank
  OS << "RC<" << DataClassName << ">\n" <<
  DataClassName << "::makeBlank() {\n"
  "  return make(RawSyntax::make(SyntaxKind::" << Kind << ",\n"
  "  {\n";
  for (const auto Child : getChildrenOf(Node)) {
    auto ChildType = getLayoutNodeType(Child);
    if (isToken(ChildType)) {
      auto ChildRec = getLayoutNodeRecord(Child);
      auto TokenKind = ChildRec->getValueAsString("Kind");
      auto TokenSpelling = ChildRec->getValueAsString("Spelling");
      OS << "    TokenSyntax::missingToken(tok::" << TokenKind << ", \"" << TokenSpelling << "\"),\n";
    } else {
      auto ChildKind = getMissingSyntaxKind(Child);
      OS << "    RawSyntax::missing(SyntaxKind::" << ChildKind << "),\n";
    }
  }
  OS << "  },\n"
  "  SourcePresence::Present));\n"
  "}\n\n";

  return false;
}

static bool printSyntaxImplementations(raw_ostream &OS) {
  auto Nodes = AllRecords->getAllDerivedDefinitions(options::Category);
  auto Any = std::string { "Any" } + options::Category;
  for (const auto *Node : Nodes) {
    if (Node->getName() == Any) {
      continue;
    }

    OS << "#pragma mark - " << Node->getName() << " API\n\n";
    printSyntaxImplementation(Node, OS);
    OS << "#pragma mark - " << Node->getName() << " Data\n\n";
    printSyntaxDataImplementation(Node, OS);
  }

  return false;
}

#pragma mark - SyntaxFactory

static bool printSyntaxFactoryInterface(raw_ostream &OS) {
  // TODO
  return false;
}


static bool printSyntaxFactoryImplementation(raw_ostream &OS) {
  // TODO
  return false;
}

#pragma mark - SyntaxRewriter

static bool printSyntaxRewriterInterface(raw_ostream &OS) {
  // TODO
  return false;
}

static bool printSyntaxRewriterImplementation(raw_ostream &OS) {
  // TODO
  return false;
}

static bool genInterface(raw_ostream &OS) {
  switch (getCategory()) {
    case Category::Decl:
    case Category::Expr:
    case Category::Stmt:
    case Category::Type:
    case Category::Pattern:
      return printSyntaxInterfaces(OS);
    case Category::SyntaxFactory:
      return printSyntaxFactoryInterface(OS);
    case Category::SyntaxRewriter:
      return printSyntaxRewriterInterface(OS);
    case Category::Unknown:
      llvm_unreachable("Unknown category given");
  }
}

static bool genImplementation(raw_ostream &OS) {
  switch (getCategory()) {
    case Category::Decl:
    case Category::Expr:
    case Category::Stmt:
    case Category::Type:
    case Category::Pattern:
      return printSyntaxImplementations(OS);
    case Category::SyntaxFactory:
      return printSyntaxFactoryImplementation(OS);
    case Category::SyntaxRewriter:
      return printSyntaxRewriterImplementation(OS);
    case Category::Unknown:
      llvm_unreachable("Unknown category given");
  }
}

static bool SyntaxTableGenMain(raw_ostream &OS, RecordKeeper &Records) {
  AllRecords = &Records;
  SyntaxCategories.insert(AllRecords->getClass("Decl"));
  SyntaxCategories.insert(AllRecords->getClass("Stmt"));
  SyntaxCategories.insert(AllRecords->getClass("Expr"));
  SyntaxCategories.insert(AllRecords->getClass("Type"));
  SyntaxCategories.insert(AllRecords->getClass("Pattern"));
  SyntaxCategories.insert(AllRecords->getClass("Token"));
  SyntaxCategories.insert(AllRecords->getClass("SyntaxCollection"));
  switch (options::Action) {
    case ActionType::None:
      llvm::errs() << "action required\n";
      llvm::cl::PrintHelpMessage();
      return true;
    case ActionType::GenInterface:
      return genInterface(OS);
    case ActionType::GenImplementation:
      return genImplementation(OS);
  }
}

int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  cl::ParseCommandLineOptions(argc, argv);
  if (getCategory() == Category::Unknown) {
    errs() << options::Category << " is an unknown category!\n";
    cl::PrintHelpMessage();
    return 1;
  }
  return TableGenMain(argv[0], &SyntaxTableGenMain);
}
