/*
 * Dark Souls - Open Server
 *
 * Injector.exe - starts the game and loads Injector.dll into it.
 *
 * This exists for the Linux loader. On Windows the loader is itself a Windows
 * program and can inject directly; on Linux it cannot, because the game is a
 * Windows process inside a Proton prefix and a native process reaches neither
 * its wineserver session nor its pressure-vessel container.
 *
 * So the Linux loader points Steam's launch options at a wrapper that swaps the
 * game executable in Proton's command line for this program. We then run inside
 * the prefix, with the same environment the game would have had, and do exactly
 * what the Windows loader does: start the game, then inject.
 *
 * Usage:  Injector.exe <game exe> [args passed through to the game]
 *
 * Both Injector.dll and Injector.config are expected next to this executable,
 * because the injector resolves its config as <dll directory>/Injector.config.
 */

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Steam's DRM is still unpacking when the process first appears, and it leaves
// the address space in a state where a remote allocation can fail. The Windows
// loader retries on the same schedule.
constexpr int kAllocAttempts = 32;
constexpr DWORD kAllocRetryDelayMs = 500;

std::wstring g_log_path;

void Log(const wchar_t* format, ...) {
  wchar_t line[2048];
  va_list args;
  va_start(args, format);
  _vsnwprintf(line, sizeof(line) / sizeof(line[0]) - 1, format, args);
  va_end(args);
  line[sizeof(line) / sizeof(line[0]) - 1] = L'\0';

  wprintf(L"[injector] %ls\n", line);
  fflush(stdout);

  if (g_log_path.empty()) {
    return;
  }
  if (FILE* file = _wfopen(g_log_path.c_str(), L"a, ccs=UTF-8")) {
    fwprintf(file, L"%ls\n", line);
    fclose(file);
  }
}

std::wstring LastErrorText() {
  const DWORD code = GetLastError();
  wchar_t* buffer = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

  std::wstring text = buffer ? buffer : L"unknown error";
  if (buffer) {
    LocalFree(buffer);
  }
  while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) {
    text.pop_back();
  }
  return L"error " + std::to_wstring(code) + L": " + text;
}

/// Directory this executable lives in, without the trailing separator.
std::wstring ModuleDirectory() {
  std::vector<wchar_t> buffer(MAX_PATH);
  DWORD length = 0;
  for (;;) {
    length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return L".";
    }
    if (length < buffer.size() - 1) {
      break;
    }
    buffer.resize(buffer.size() * 2);
  }

  std::wstring path(buffer.data(), length);
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"." : path.substr(0, separator);
}

/// Converts a Unix path to one the Windows side understands.
///
/// The wrapper script hands us the game path exactly as Steam wrote it, which
/// is a Linux path. Wine converts the program it launches but not the arguments
/// it passes on, so the conversion has to happen here.
std::wstring ToWindowsPath(const std::wstring& path) {
  if (path.empty() || path[0] != L'/') {
    return path;  // Already a Windows path.
  }

  // Wine exports a proper converter that honours the prefix's drive mappings.
  using WineGetDosFileName = WCHAR*(CDECL*)(const char*);
  if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
    auto convert = reinterpret_cast<WineGetDosFileName>(
        reinterpret_cast<void*>(GetProcAddress(kernel, "wine_get_dos_file_name")));
    if (convert) {
      const int size =
          WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
      std::vector<char> utf8(size > 0 ? size : 1);
      WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), size, nullptr, nullptr);

      if (WCHAR* converted = convert(utf8.data())) {
        std::wstring result = converted;
        HeapFree(GetProcessHeap(), 0, converted);
        if (!result.empty()) {
          return result;
        }
      }
    }
  }

  // Every Wine prefix maps Z: to the filesystem root, so this is a safe fallback.
  std::wstring result = L"Z:" + path;
  for (wchar_t& character : result) {
    if (character == L'/') {
      character = L'\\';
    }
  }
  return result;
}

std::wstring DirectoryOf(const std::wstring& path) {
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"." : path.substr(0, separator);
}

/// Quotes an argument so CreateProcessW splits the command line the way we mean.
std::wstring Quote(const std::wstring& argument) {
  if (argument.find_first_of(L" \t\"") == std::wstring::npos) {
    return argument;
  }

  std::wstring quoted = L"\"";
  size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      backslashes++;
      quoted.push_back(character);
      continue;
    }
    if (character == L'"') {
      quoted.append(backslashes + 1, L'\\');
    }
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

/// Writes the DLL path into the target and runs LoadLibraryW on it there.
bool InjectLibrary(HANDLE process, const std::wstring& dll_path) {
  HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
  if (!kernel) {
    Log(L"could not resolve kernel32.dll: %ls", LastErrorText().c_str());
    return false;
  }

  FARPROC load_library = GetProcAddress(kernel, "LoadLibraryW");
  if (!load_library) {
    Log(L"could not resolve LoadLibraryW: %ls", LastErrorText().c_str());
    return false;
  }

  const SIZE_T bytes = (dll_path.size() + 1) * sizeof(wchar_t);

  void* remote = nullptr;
  for (int attempt = 0; attempt < kAllocAttempts && remote == nullptr; attempt++) {
    remote = VirtualAllocEx(process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remote) {
      Sleep(kAllocRetryDelayMs);
    }
  }
  if (!remote) {
    Log(L"could not allocate %llu bytes in the game: %ls", (unsigned long long)bytes,
        LastErrorText().c_str());
    return false;
  }

  SIZE_T written = 0;
  if (!WriteProcessMemory(process, remote, dll_path.c_str(), bytes, &written) || written != bytes) {
    Log(L"could not write the dll path into the game: %ls", LastErrorText().c_str());
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return false;
  }

  // LoadLibraryW takes one pointer and returns a handle, which is layout
  // compatible with a thread routine. The compiler cannot know that, so the
  // cast goes through void* to say it is deliberate.
  auto entry_point =
      reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<void*>(load_library));

  HANDLE thread = CreateRemoteThread(process, nullptr, 0, entry_point, remote, 0, nullptr);
  if (!thread) {
    Log(L"could not start the loader thread in the game: %ls", LastErrorText().c_str());
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return false;
  }

  WaitForSingleObject(thread, INFINITE);

  DWORD module_handle = 0;
  GetExitCodeThread(thread, &module_handle);
  CloseHandle(thread);
  VirtualFreeEx(process, remote, 0, MEM_RELEASE);

  if (module_handle == 0) {
    Log(L"the game refused to load the dll; check that it matches the game's architecture");
    return false;
  }

  Log(L"injected, module handle 0x%08lx", module_handle);
  return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const std::wstring here = ModuleDirectory();
  g_log_path = here + L"\\DS2OS_Injector.log";

  Log(L"---- ds2os injector ----");

  if (argc < 2) {
    Log(L"usage: Injector.exe <game exe> [args]");
    return 2;
  }

  const std::wstring game = ToWindowsPath(argv[1]);
  const std::wstring game_directory = DirectoryOf(game);
  const std::wstring dll = here + L"\\Injector.dll";

  Log(L"game   %ls", game.c_str());
  Log(L"dll    %ls", dll.c_str());

  if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
    Log(L"Injector.dll is not next to this executable; nothing to inject");
    return 3;
  }

  std::wstring command_line = Quote(game);
  for (int index = 2; index < argc; index++) {
    command_line += L" " + Quote(argv[index]);
  }

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};

  std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
  mutable_command_line.push_back(L'\0');

  if (!CreateProcessW(game.c_str(), mutable_command_line.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, game_directory.c_str(), &startup, &process)) {
    Log(L"could not start the game: %ls", LastErrorText().c_str());
    return 4;
  }

  Log(L"started the game, pid %lu", process.dwProcessId);

  // A failed injection is not a reason to deny the user their game: it just
  // means they are playing on the retail servers, which the log will say.
  if (!InjectLibrary(process.hProcess, dll)) {
    Log(L"continuing without the injector; the game will use the retail servers");
  }

  // Steam waits on this process, so it has to outlive the game.
  WaitForSingleObject(process.hProcess, INFINITE);

  DWORD exit_code = 0;
  GetExitCodeProcess(process.hProcess, &exit_code);
  Log(L"the game exited with code %lu", exit_code);

  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return static_cast<int>(exit_code);
}
