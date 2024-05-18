#include "crashhandler.h"

#include <inttypes.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#include <winbase.h>
#include <dbghelp.h>
#include <psapi.h>
#else
#ifdef MACOSX
#include <mach-o/dyld.h>
#else
#include <link.h>
#endif
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <err.h>
#include <dlfcn.h>
#endif

#include "tgl.h"
#include "tapp.h"
#include "tenv.h"
#include "tconvert.h"
#include "texception.h"
#include "tfilepath_io.h"
#include "toonz/toonzfolders.h"
#include "toonz/tproject.h"
#include "toonz/tscenehandle.h"
#include "toonz/toonzscene.h"
#include "tsystem.h"

#include <QOperatingSystemVersion>
#include <QDesktopServices>
#include <QApplication>
#include <QClipboard>
#include <QThread>
#include <QMainWindow>
#include <QMessageBox>
#include <QCloseEvent>
#include <QDialog>
#include <QLayout>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>

#include <QDebug>

static QWidget *s_parentWindow = NULL;
static bool s_reportProjInfo   = false;
#ifdef _WIN32
static PEXCEPTION_POINTERS s_exceptionPtr = NULL;
#endif

//-----------------------------------------------------------------------------

static const char *filenameOnly(const char *path) {
  for (int i = strlen(path); i >= 0; --i) {
    if (path[i] == '\\' || path[i] == '/') return path + i + 1;
  }
  return path;
}

//-----------------------------------------------------------------------------
static QString obfuscateUsername(QString path) {
  QString username = TSystem::getUserName();
  QString newPath  = path;
  newPath.replace("/" + username + "/", "/USER/");
  newPath.replace("\\" + username + "\\", "\\USER\\");
  return newPath;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// Windows platform functions

#ifdef _WIN32

#define HAS_MINIDUMP
static bool generateMinidump(TFilePath dumpFile,
                             PEXCEPTION_POINTERS exceptionInfo) {
  HANDLE hDumpFile = CreateFileW(dumpFile.getWideString().c_str(),
                                 GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
  if (hDumpFile == INVALID_HANDLE_VALUE) return false;

  MINIDUMP_EXCEPTION_INFORMATION mdei;
  mdei.ThreadId          = GetCurrentThreadId();
  mdei.ExceptionPointers = exceptionInfo;
  mdei.ClientPointers    = TRUE;

  if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile,
                        MiniDumpNormal, &mdei, 0, NULL)) {
    CloseHandle(hDumpFile);
    return true;
  }

  return false;
}

#define HAS_MODULES
static void printModules(std::string &out) {
  HANDLE hProcess = GetCurrentProcess();

  HMODULE modules[1024];
  DWORD size;
  if (EnumProcessModules(hProcess, modules, sizeof(modules), &size)) {
    for (unsigned int i = 0; i < size / sizeof(HMODULE); i++) {
      char moduleName[512];
      GetModuleFileNameA(modules[i], moduleName, 512);
      QString path = obfuscateUsername(QString(moduleName));
      out.append(path.toStdString());
      out.append("\n");
    }
  }
}

#define HAS_BACKTRACE
static void printBacktrace(std::string &out) {
  int frameStack = 0;
  int frameSkip  = 3;

  HANDLE hProcess = GetCurrentProcess();

  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_INCLUDE_32BIT_MODULES |
                SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_LOAD_LINES |
                SYMOPT_UNDNAME);

  SymInitialize(hProcess, NULL, TRUE);

  CONTEXT context;
  RtlCaptureContext(&context);

  char sourceSymMem[sizeof(IMAGEHLP_SYMBOL64) + 1025];
  PIMAGEHLP_SYMBOL64 sourceSym = (PIMAGEHLP_SYMBOL64)&sourceSymMem;
  memset(sourceSymMem, 0, sizeof(sourceSymMem));

  IMAGEHLP_LINE64 sourceInfo;
  memset(&sourceInfo, 0, sizeof(IMAGEHLP_LINE64));
  sourceInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

  IMAGEHLP_MODULE64 moduleInfo;
  memset(&moduleInfo, 0, sizeof(moduleInfo));
  moduleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);

  STACKFRAME64 stackframe;
  memset(&stackframe, 0, sizeof(STACKFRAME64));

#ifdef _WIN64
  int machineType             = IMAGE_FILE_MACHINE_AMD64;
  stackframe.AddrPC.Offset    = context.Rip;
  stackframe.AddrPC.Mode      = AddrModeFlat;
  stackframe.AddrStack.Offset = context.Rsp;
  stackframe.AddrStack.Mode   = AddrModeFlat;
  stackframe.AddrFrame.Offset = context.Rbp;
  stackframe.AddrFrame.Mode   = AddrModeFlat;
#else
  int machineType             = IMAGE_FILE_MACHINE_I386;
  stackframe.AddrPC.Offset    = context.Eip;
  stackframe.AddrPC.Mode      = AddrModeFlat;
  stackframe.AddrStack.Offset = context.Esp;
  stackframe.AddrStack.Mode   = AddrModeFlat;
  stackframe.AddrFrame.Offset = context.Ebp;
  stackframe.AddrFrame.Mode   = AddrModeFlat;
#endif

  HANDLE hThread = GetCurrentThread();
  while (StackWalk64(machineType, hProcess, hThread, &stackframe, &context,
                     NULL, SymFunctionTableAccess64, SymGetModuleBase64,
                     NULL)) {
    // Skip first frames since they point to this function
    if (frameStack++ < frameSkip) continue;
    char numStr[32];
    memset(numStr, 0, sizeof(numStr));
    sprintf(numStr, "%3i> ", frameStack - frameSkip);
    out.append(numStr);

    sourceSym->SizeOfStruct  = sizeof(IMAGEHLP_SYMBOL64);
    sourceSym->MaxNameLength = 1024;

    // Get symbol name
    DWORD64 displacement64;
    if (SymGetSymFromAddr64(hProcess, (ULONG64)stackframe.AddrPC.Offset,
                            &displacement64, sourceSym)) {
      out.append(sourceSym->Name);

      // Get module filename
      char moduleFile[512];
      MEMORY_BASIC_INFORMATION mbi;
      VirtualQuery((LPCVOID)stackframe.AddrPC.Offset, &mbi, sizeof(mbi));
      GetModuleFileNameA((HMODULE)mbi.AllocationBase, moduleFile, 512);

      // Get source filename and line
      DWORD displacement32;
      if (SymGetLineFromAddr64(hProcess, stackframe.AddrPC.Offset,
                               &displacement32, &sourceInfo) != FALSE) {
        out.append(" {");
        out.append(filenameOnly(sourceInfo.FileName));
        out.append(":");
        out.append(std::to_string(sourceInfo.LineNumber));
        out.append("}");
      } else {
        memset(numStr, 0, sizeof(numStr));
        sprintf(numStr, " [0x%" PRIx64 "]", stackframe.AddrPC.Offset);
        out.append(numStr);
      }
      out.append(" <");
      out.append(filenameOnly(moduleFile));
      out.append(">");
    }

    out.append("\n");
  }

  SymCleanup(hProcess);
}

//-----------------------------------------------------------------------------

LONG WINAPI exceptionHandler(PEXCEPTION_POINTERS info) {
  static volatile bool handling = false;

  const char *reason = "Unknown";
  switch (info->ExceptionRecord->ExceptionCode) {
  case EXCEPTION_ACCESS_VIOLATION:
    reason = "EXCEPTION_ACCESS_VIOLATION";
    break;
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    reason = "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    break;
  case EXCEPTION_DATATYPE_MISALIGNMENT:
    reason = "EXCEPTION_DATATYPE_MISALIGNMENT";
    break;
  case EXCEPTION_FLT_DENORMAL_OPERAND:
    reason = "EXCEPTION_FLT_DENORMAL_OPERAND";
    break;
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    reason = "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    break;
  case EXCEPTION_FLT_INEXACT_RESULT:
    reason = "EXCEPTION_FLT_INEXACT_RESULT";
    break;
  case EXCEPTION_FLT_INVALID_OPERATION:
    reason = "EXCEPTION_FLT_INVALID_OPERATION";
    break;
  case EXCEPTION_FLT_OVERFLOW:
    reason = "EXCEPTION_FLT_OVERFLOW";
    break;
  case EXCEPTION_FLT_STACK_CHECK:
    reason = "EXCEPTION_FLT_STACK_CHECK";
    break;
  case EXCEPTION_FLT_UNDERFLOW:
    reason = "EXCEPTION_FLT_UNDERFLOW";
    break;
  case EXCEPTION_ILLEGAL_INSTRUCTION:
    reason = "EXCEPTION_ILLEGAL_INSTRUCTION";
    break;
  case EXCEPTION_IN_PAGE_ERROR:
    reason = "EXCEPTION_IN_PAGE_ERROR";
    break;
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
    reason = "EXCEPTION_INT_DIVIDE_BY_ZERO";
    break;
  case EXCEPTION_INVALID_DISPOSITION:
    reason = "EXCEPTION_INVALID_DISPOSITION";
    break;
  case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    reason = "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    break;
  case EXCEPTION_PRIV_INSTRUCTION:
    reason = "EXCEPTION_PRIV_INSTRUCTION";
    break;
  case EXCEPTION_STACK_OVERFLOW:
    reason = "EXCEPTION_STACK_OVERFLOW";
    break;
  default:
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Avoid new exceptions inside the crash handler
  if (handling) return EXCEPTION_CONTINUE_SEARCH;

  handling = true;
  s_exceptionPtr = info;
  if (CrashHandler::trigger(reason, true)) _Exit(1);
  handling = false;

  return EXCEPTION_CONTINUE_SEARCH;
}

#endif

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// Linux and Mac OS X platform functions

#ifndef _WIN32

static bool sh(std::string &out, const char *cmd) {
  char buffer[128];
  FILE *p = popen(cmd, "r");
  if (p == NULL) return false;
  while (fgets(buffer, 128, p)) out.append(buffer);
  pclose(p);
  return true;
}

//-----------------------------------------------------------------------------

static bool addr2line(std::string &out, const char *exepath, const size_t addr,
                      const size_t laddr) {
  char cmd[512];
#ifdef MACOSX
  sprintf(cmd, "atos -o \"%.400s\" -l %p %p 2>&1", exepath, laddr, addr);
#else
  sprintf(cmd, "addr2line -fpCis -e \"%.400s\" %p 2>&1", exepath, addr);
#endif
  return sh(out, cmd);
}

//-----------------------------------------------------------------------------

static bool generateMinidump(TFilePath dumpFile) { return false; }

//-----------------------------------------------------------------------------

#define HAS_MODULES
#ifdef LINUX
static int printModuleInfo(struct dl_phdr_info *i, size_t size, void *data) {
  std::string *out = (std::string *)data;
  char addr[512];
  sprintf(addr, "%p", (long)i->dlpi_addr);
  out->append(std::string(addr) + " - " + std::string(i->dlpi_name) + "\n");
  return 0;
}
#endif

static void printModules(std::string &out) {
#ifdef MACOSX
  char str[512];
  int c = _dyld_image_count();
  for (int i = 0; i < c; i++) {
    const char *image_name           = _dyld_get_image_name(i);
    const struct mach_header *header = _dyld_get_image_header(i);
    uintptr_t loadaddr               = (uintptr_t)header;
    sprintf(str, "%p", loadaddr);

    QString path = obfuscateUsername(QString(image_name));
    out.append(std::string(str) + " - " + path.toStdString());
    out.append("\n");
  }
#else
  dl_iterate_phdr(printModuleInfo, &out);
#endif
}

//-----------------------------------------------------------------------------

#define HAS_BACKTRACE
static void printBacktrace(std::string &out) {
  int frameStack = 0;
  int frameSkip  = 3;

  const int size = 256;
  void *buffer[size];

  // Back trace
  int nptrs = backtrace(buffer, size);
  for (int i = 0; i < nptrs; ++i) {
    // Skip first frames since they point to this function
    if (frameStack++ < frameSkip) continue;
    char numStr[32];
    memset(numStr, 0, sizeof(numStr));
    sprintf(numStr, "%3i> ", frameStack - frameSkip);
    out.append(numStr);

    std::string line;
    std::string sym    = "??? at ??:?";
    std::string module = "???";
    size_t addr        = (size_t)buffer[i];

    bool found = false;

    Dl_info info;
    int infoFound = dladdr(buffer[i], &info);

    if (infoFound) module = std::string(info.dli_fname);

#ifdef MACOSX
    if (module.find("Tahoma2D.app/Contents/MacOS") != std::string::npos) {
      std::string dsym = module;
      int pos          = dsym.find("Contents");
      dsym.replace(pos, 14, "DSYM/Contents/Resources/DWARF");
      TFilePath file(dsym);
      if (TFileStatus(file).doesExist()) module = dsym;
    }

    char addrstr[512];
    sprintf(addrstr, "%p", addr);
    sym = std::string(addrstr) + " (in " + module + ")";
#else
    link_map *link_map;
    dladdr1(buffer[i], &info, (void **)&link_map, RTLD_DL_LINKMAP);
    addr -= link_map->l_addr;
#endif

    if (infoFound &&
        addr2line(line, module.c_str(), addr, (size_t)info.dli_fbase)) {
      found = (line.rfind("??", 0) != 0) &&
              (line.find("cannot load symbol", 0) != 0) &&
              (line.find("command not found") != 0);
    }

    out.append(found ? line : (sym + "\n"));
  }
}

void signalHandler(int sig) {
  static volatile bool handling = false;

  const char *reason = "Unknown";
  switch (sig) {
  case SIGABRT:
    reason = "(SIGABRT) Usually caused by an abort() or assert()";
    break;
  case SIGFPE:
    reason = "(SIGFPE) Arithmetic exception, such as divide by zero";
    break;
  case SIGILL:
    reason = "(SIGILL) Illegal instruction";
    break;
  case SIGINT:
    reason = "(SIGINT) Interactive attention signal, (usually ctrl+c)";
    break;
  case SIGSEGV:
    reason = "(SIGSEGV) Segmentation Fault";
    break;
  case SIGTERM:
    reason = "(SIGTERM) A termination request was sent to the program";
    break;
  }

  // Avoid new signals inside the crash handler
  if (handling) return;

  handling = true;
  if (CrashHandler::trigger(reason, true)) _Exit(1);
  handling = false;
}

#endif

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

static void printSysInfo(std::string &out) {
  out.append("Build ABI: " + QSysInfo::buildAbi().toStdString() + "\n");
  out.append("Operating System: " + QSysInfo::prettyProductName().toStdString() + "\n");
  out.append("OS Kernel: " + QSysInfo::kernelVersion().toStdString() + "\n");
  out.append("CPU Threads: " + std::to_string(QThread::idealThreadCount()) + "\n");
}

//-----------------------------------------------------------------------------

static void printGPUInfo(std::string &out) {
  const char *gpuVendorName = (const char *)glGetString(GL_VENDOR);
  const char *gpuModelName  = (const char *)glGetString(GL_RENDERER);
  const char *gpuVersion    = (const char *)glGetString(GL_VERSION);
  if (gpuVendorName)
    out.append("GPU Vendor: " + std::string(gpuVendorName) + "\n");
  if (gpuModelName)
    out.append("GPU Model: " + std::string(gpuModelName) + "\n");
  if (gpuVersion)
    out.append("GPU Version: " + std::string(gpuVersion) + "\n");
}

//-----------------------------------------------------------------------------

CrashHandler::CrashHandler(QWidget *parent, TFilePath crashFile, QString crashReport)
    : QDialog(parent), m_crashFile(crashFile), m_crashReport(crashReport) {
  setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::CustomizeWindowHint);

  QStringList sl;
  sl.append(tr("<b>Tahoma2D crashed unexpectedly.</b>"));
  sl.append("");
  sl.append(tr("A crash report has been generated."));
  sl.append(
      tr("To report, click 'Open Issue Webpage' to access Tahoma2D's Issues "
         "page on GitHub."));
  sl.append(tr("Click on the 'New issue' button and fill out the form."));
  sl.append("");
  sl.append(tr("System Configuration and Problem Details:"));
  
  QLabel *headtext = new QLabel(sl.join("<br>"));
  headtext->setTextFormat(Qt::RichText);

  QTextEdit *reportTxt = new QTextEdit();
  reportTxt->setText(crashReport);
  reportTxt->setReadOnly(true);
  reportTxt->setLineWrapMode(QTextEdit::LineWrapMode::NoWrap);
  reportTxt->setStyleSheet(
      "background:white;\ncolor:black;\nborder:1 solid black;");

  QVBoxLayout *mainLayout = new QVBoxLayout();
  QHBoxLayout *buttonsLay = new QHBoxLayout();

  QPushButton *copyBtn   = new QPushButton(tr("Copy to Clipboard"));
  QPushButton *webBtn    = new QPushButton(tr("Open Issue Webpage"));
  QPushButton *folderBtn = new QPushButton(tr("Open Reports Folder"));
  QPushButton *closeBtn  = new QPushButton(tr("Close Application"));
  buttonsLay->addWidget(copyBtn);
  buttonsLay->addWidget(webBtn);
  buttonsLay->addWidget(folderBtn);
  buttonsLay->addWidget(closeBtn);

  mainLayout->addWidget(headtext);
  mainLayout->addWidget(reportTxt);
  mainLayout->addLayout(buttonsLay);

  bool ret = connect(copyBtn, SIGNAL(clicked()), this, SLOT(copyClipboard()));
  ret = ret && connect(webBtn, SIGNAL(clicked()), this, SLOT(openWebpage()));
  ret = ret && connect(folderBtn, SIGNAL(clicked()), this, SLOT(openFolder()));
  ret = ret && connect(closeBtn, SIGNAL(clicked()), this, SLOT(accept()));
  if (!ret) throw TException();

  setWindowTitle(tr("Tahoma2D crashed!"));
  setLayout(mainLayout);
}

void CrashHandler::reject() {
/*
  QStringList sl;
  sl.append(tr("Application is in unstable state and must be restarted."));
  sl.append(tr("Resuming is not recommended and may lead to an unrecoverable crash."));
  sl.append(tr("Ignore advice and try to resume program?"));

  QMessageBox::StandardButton reply =
      QMessageBox::question(this, tr("Ignore crash?"), sl.join("\n"),
                            QMessageBox::Yes | QMessageBox::No);
  if (reply == QMessageBox::Yes) {
    QDialog::reject();
  }
*/
}

//-----------------------------------------------------------------------------

void CrashHandler::copyClipboard() {
  QClipboard *clipboard = QApplication::clipboard();
  clipboard->setText(m_crashReport);
}

//-----------------------------------------------------------------------------

void CrashHandler::openWebpage() {
  QDesktopServices::openUrl(QUrl("https://github.com/tahoma2d/tahoma2d/issues"));
}  

//-----------------------------------------------------------------------------

void CrashHandler::openFolder() {
  TFilePath fp = ToonzFolder::getCrashReportFolder();
  QDesktopServices::openUrl(QUrl("file:///" + fp.getQString()));
}

//-----------------------------------------------------------------------------

void CrashHandler::install() {
#ifdef _WIN32
  // std library seems to override this
  //SetUnhandledExceptionFilter(exceptionHandler);

  void *handler = AddVectoredExceptionHandler(0, exceptionHandler);
  assert(handler != NULL);
  //RemoveVectoredExceptionHandler(handler);
#else
  signal(SIGABRT, signalHandler);
  signal(SIGFPE, signalHandler);
  signal(SIGILL, signalHandler);
  signal(SIGINT, signalHandler);
  signal(SIGSEGV, signalHandler);
  signal(SIGTERM, signalHandler);
#endif
}

//-----------------------------------------------------------------------------

void CrashHandler::reportProjectInfo(bool enableReport) {
  s_reportProjInfo = enableReport;
}

//-----------------------------------------------------------------------------

void CrashHandler::attachParentWindow(QWidget *parent) {
  s_parentWindow = parent;
}

//-----------------------------------------------------------------------------

bool CrashHandler::trigger(const QString reason, bool showDialog) {
  char fileName[128];
  char dumpName[128];
  char dateName[128];
  std::string out;


  // Get time and build filename
  time_t acc_time;
  time(&acc_time);
  struct tm *tm = localtime(&acc_time);
  strftime(dateName, 128, "%Y-%m-%d %H:%M:%S", tm);
  strftime(fileName, 128, "Crash-%Y%m%d-%H%M%S.log", tm);
  strftime(dumpName, 128, "Crash-%Y%m%d-%H%M%S.dmp", tm);
  TFilePath fpCrsh = ToonzFolder::getCrashReportFolder() + fileName;
  TFilePath fpDump = ToonzFolder::getCrashReportFolder() + dumpName;

  // Generate minidump
#ifdef _WIN32
  bool minidump = generateMinidump(fpDump, s_exceptionPtr);
#else
  bool minidump = generateMinidump(fpDump);
#endif

  // Generate report
  try {
    out.append(TEnv::getApplicationFullName() + "  (Build " + __DATE__ ")\n");
    out.append("\nReport Date: ");
    out.append(dateName);
    out.append("\nCrash Reason: ");
    out.append(reason.toStdString());
    out.append("\n\n");
    printSysInfo(out);
    out.append("\n");
    printGPUInfo(out);
    out.append("\nCrash File: ");
    out.append(obfuscateUsername(fpCrsh.getQString()).toStdString());
#ifdef HAS_MINIDUMP
    out.append("\nMini Dump File: ");
    if (minidump)
      out.append(obfuscateUsername(fpDump.getQString()).toStdString());
    else
      out.append("Failed");
#endif
    out.append("\n");
  } catch (...) {
  }
  try {
    if (s_reportProjInfo) {
      TProjectManager *pm = TProjectManager::instance();
      TApp *app           = TApp::instance();

      TProjectP currentProject = pm->getCurrentProject();
      TFilePath projectPath    = currentProject->getProjectPath();

      ToonzScene *currentScene = app->getCurrentScene()->getScene();
      std::wstring sceneName   = currentScene->getSceneName();

      out.append("\nApplication Dir: ");
      out.append(obfuscateUsername(QCoreApplication::applicationDirPath())
                     .toStdString());
      out.append("\nStuff Dir: ");
      out.append(
          obfuscateUsername(TEnv::getStuffDir().getQString()).toStdString());
      out.append("\n");
      out.append("\nProject Name: ");
      out.append(currentProject->getName().getQString().toStdString());
      out.append("\nScene Name: ");
      out.append(QString::fromStdWString(sceneName).toStdString());
      out.append("\nProject Path: ");
      out.append(obfuscateUsername(projectPath.getQString()).toStdString());
      out.append("\nScene Path: ");
      out.append(obfuscateUsername(currentScene->getScenePath().getQString())
                     .toStdString());
      out.append("\n");
    }
  } catch (...) {
  }
#ifdef HAS_BACKTRACE
  try {
    out.append("\n==== Backtrace ====\n");
    printBacktrace(out);
    out.append("==== End ====\n");
  } catch (...) {
  }
#endif
#ifdef HAS_MODULES
  try {
    out.append("\n==== Modules ====\n");
    printModules(out);
    out.append("==== End ====\n");
  } catch (...) {
  }
#endif

  // Save to crash information to file
  FILE *fw = fopen(fpCrsh, "w");
  if (fw != NULL) {
    fwrite(out.c_str(), 1, out.size(), fw);
    fclose(fw);
  }

  if (showDialog) {
    // Show crash handler dialog
    CrashHandler crashdialog(s_parentWindow, fpCrsh, QString::fromStdString(out));
    return crashdialog.exec() != QDialog::Rejected;
  }

  return true;
}

//-----------------------------------------------------------------------------
