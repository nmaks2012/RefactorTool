#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

#include <unordered_set>

#include "RefactorTool.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

static llvm::cl::OptionCategory ToolCategory("refactor-tool options");

// Метод run вызывается для каждого совпадения с матчем.
// Мы проверяем тип совпадения по bind-именам и применяем рефакторинг.
void RefactorHandler::run(const MatchFinder::MatchResult &Result) {
  auto &Diag = Result.Context->getDiagnostics();
  auto &SM =
      *Result.SourceManager; // Получаем SourceManager для проверки isInMainFile

  if (const auto *Dtor =
          Result.Nodes.getNodeAs<CXXDestructorDecl>("nonVirtualDtor")) {
    handle_nv_dtor(Dtor, Diag, SM);
  }

  if (const auto *Method =
          Result.Nodes.getNodeAs<CXXMethodDecl>("missingOverride");
      Method && Method->size_overridden_methods() > 0 &&
      !Method->hasAttr<OverrideAttr>() && 
      !isa<CXXDestructorDecl>(Method) // Проверка что метод не является и деструктором
    ) {
    handle_miss_override(Method, Diag, SM, *Result.Context);
  }

  if (const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("loopVar")) {
    handle_crange_for(LoopVar, Diag, SM);
  }
}

void RefactorHandler::handle_nv_dtor(const CXXDestructorDecl *Dtor,
                                     DiagnosticsEngine &Diag,
                                     SourceManager &SM) {

  // Получаем начальную позицию метода для проверки и использования в качестве
  // ключа.
  SourceLocation DtorStartLoc = Dtor->getBeginLoc();

  // Проверяем, что совпадение относится к основному файлу
  if (!SM.isInMainFile(DtorStartLoc)) {
    return;
  }

  // Избегаем дублирования правок для одного и того же деструктора.
  unsigned LocAsInt = DtorStartLoc.getRawEncoding();
  if (processedLocations.count(LocAsInt)) {
    return;
  }

  // Выполняем рефакторинг, вставляя "virtual ".
  Rewrite.InsertTextBefore(DtorStartLoc, "virtual ");

  // Сообщаем о выполненном действии.
  const unsigned DiagID = Diag.getCustomDiagID(
      DiagnosticsEngine::Remark, "добавлено 'virtual' к деструктору '%0'");
  Diag.Report(DtorStartLoc, DiagID) << Dtor->getNameAsString();

  // Отмечаем позицию как обработанную
  processedLocations.insert(LocAsInt);
}

void RefactorHandler::handle_miss_override(const CXXMethodDecl *Method,
                                           DiagnosticsEngine &Diag,
                                           SourceManager &SM,
                                           ASTContext &Context) {

  // Получаем начальную позицию метода для проверки и использования в качестве
  // ключа.
  SourceLocation MethodStartLoc = Method->getBeginLoc();

  // Проверяем, что совпадение относится к основному файлу
  if (!SM.isInMainFile(MethodStartLoc)) {
    return;
  }

  // Избегаем дублирования правок для одного и того же метода.
  unsigned LocAsInt = MethodStartLoc.getRawEncoding();
  if (processedLocations.count(LocAsInt)) {
    return;
  }

  // Получаем информацию о типе функции и ее расположении в коде.
  TypeLoc FuncTypeLoc = Method->getTypeSourceInfo()->getTypeLoc();
  // Находим конец этой части (включая 'const', 'noexcept' и т.д.).
  SourceLocation EndOfDeclarator = FuncTypeLoc.getEndLoc();

  // Используем Lexer, чтобы найти точную позицию *после* последнего токена
  // декларатора.
  SourceLocation InsertionPoint =
      Lexer::getLocForEndOfToken(EndOfDeclarator, 0, SM, Context.getLangOpts());

  // Если что-то пошло не так и мы не смогли найти точку вставки, выходим.
  if (InsertionPoint.isInvalid()) {
    return;
  }

  // Выполняем рефакторинг, вставляя " override".
  Rewrite.InsertText(InsertionPoint, " override");

  // Сообщаем о выполненном действии.
  const unsigned DiagID = Diag.getCustomDiagID(
      DiagnosticsEngine::Remark, "добавлено 'override' к методу '%0'");
  Diag.Report(MethodStartLoc, DiagID) << Method->getNameAsString();

  // Отмечаем позицию как обработанную
  processedLocations.insert(LocAsInt);
}

void RefactorHandler::handle_crange_for(const VarDecl *LoopVar,
                                        DiagnosticsEngine &Diag,
                                        SourceManager &SM) {
  // Получаем начальную позицию метода для проверки и использования в качестве
  // ключа.
  SourceLocation VarNameLoc = LoopVar->getLocation();

  // Проверяем, что совпадение относится к основному файлу
  if (!SM.isInMainFile(VarNameLoc)) {
    return;
  }

  // Избегаем дублирования правок для одной и той же переменной.
  unsigned LocAsInt = VarNameLoc.getRawEncoding();
  if (processedLocations.count(LocAsInt)) {
    return;
  }

  // Выполняем рефакторинг, вставляя "& ".
  Rewrite.InsertTextBefore(VarNameLoc, "& ");

  // Сообщаем о выполненном действии, объясняя причину.
  const unsigned DiagID = Diag.getCustomDiagID(
      DiagnosticsEngine::Remark,
      "добавлена ссылка ('&') к переменной цикла '%0' для предотвращения лишнего копирования");
  Diag.Report(VarNameLoc, DiagID) << LoopVar->getNameAsString();

  // Отмечаем позицию как обработанную
  processedLocations.insert(LocAsInt);
}

auto NvDtorMatcher() {
  // Матчер находит наследника (`isDerivedFrom`),
  // чтобы затем проверить свойства его базового класса.
  return cxxRecordDecl(isDerivedFrom(cxxRecordDecl(
      // Отбрасываем совпадения найденные в системных файлах
      unless(isExpansionInSystemHeader()), isDefinition(),
      has(cxxDestructorDecl(
              // `isUserProvided()` отсеивает неявные деструкторы,
              // а `unless(isVirtual())` находит саму проблему.
              unless(isVirtual()), unless(isImplicit()))
              .bind("nonVirtualDtor")))));
}

auto NoOverrideMatcher() {
  return cxxMethodDecl(
             // `isOverride()` — основной матчер, который
             // выполняет всю сложную работу по проверке иерархии и наличия
             // виртуального метода в базовом классе.
             isOverride(),
             // Отбрасываем совпадения найденные в системных файлах
             unless(isExpansionInSystemHeader()), unless(isImplicit()))
      // Сам матчер находит всех кандидатов. Финальная проверка
      // на отсутствие 'override' происходит в C++ коде для совместимости со
      // старыми версиями Clang.
      .bind("missingOverride");
}

auto NoRefConstVarInRangeLoopMatcher() {
  return cxxForRangeStmt(
      hasLoopVariable(varDecl(
                          // Комбинация трех проверок типа переменной.
                          // `isConstQualified()` находит `const`.
                          hasType(isConstQualified()),
                          // `unless(hasType(referenceType()))` находит передачу
                          // по значению (отсутствие `&`).
                          unless(hasType(referenceType())),
                          // `recordType()` отфильтровывает дешевые для
                          // копирования примитивные типы.
                          hasType(hasCanonicalType(recordType())))
                          .bind("loopVar")),
      // Отбрасываем совпадения найденные в системных файлах
      unless(isExpansionInSystemHeader()));
}

// Конструктор принимает Rewriter для изменения кода.
ComplexConsumer::ComplexConsumer(Rewriter &Rewrite) : Handler(Rewrite) {
  // Создаем MatchFinder и добавляем матчеры.
  Finder.addMatcher(NvDtorMatcher(), &Handler);
  Finder.addMatcher(NoOverrideMatcher(), &Handler);
  Finder.addMatcher(NoRefConstVarInRangeLoopMatcher(), &Handler);
}

// Метод HandleTranslationUnit вызывается для каждого файла.
void ComplexConsumer::HandleTranslationUnit(ASTContext &Context) {
  Finder.matchAST(Context);
}

std::unique_ptr<ASTConsumer>
CodeRefactorAction::CreateASTConsumer(CompilerInstance &CI, StringRef file) {
  RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
  return std::make_unique<ComplexConsumer>(RewriterForCodeRefactor);
}

bool CodeRefactorAction::BeginSourceFileAction(CompilerInstance &CI) {
  // Инициализируем Rewriter для рефакторинга.
  RewriterForCodeRefactor.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
  return true; // Возвращаем true, чтобы продолжить обработку файла.
}

void CodeRefactorAction::EndSourceFileAction() {
  // Применяем изменения в файле.
  if (RewriterForCodeRefactor.overwriteChangedFiles()) {
    llvm::errs() << "Error applying changes to files.\n";
  }
}