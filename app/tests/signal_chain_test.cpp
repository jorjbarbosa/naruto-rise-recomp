#include <rex/exception_handler.h>
#include <csignal>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

namespace {
bool Recover(rex::arch::Exception*, void*) { return true; }
bool Decline(rex::arch::Exception*, void*) { return false; }
void Previous(int number, siginfo_t* info, void* context) {
  _exit(info && context && info->si_signo == number ? 42 : 43);
}
bool Run(int number, int kind) {
  const pid_t child = fork();
  if (child < 0) return false;
  if (!child) {
    alarm(3); // A swallowed fatal signal must fail promptly, never hang CI.
    struct sigaction prior {};
    prior.sa_sigaction = Previous;
    prior.sa_flags = SA_SIGINFO;
    sigemptyset(&prior.sa_mask);
    if (kind != 2) sigaction(number, &prior, nullptr);
    else signal(number, SIG_DFL);
    rex::arch::ExceptionHandler::Install(kind == 0 ? Recover : Decline, nullptr);
    raise(number);
    _exit(0);
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child) return false;
  if (kind == 2) return WIFSIGNALED(status) && WTERMSIG(status) == number;
  return WIFEXITED(status) && WEXITSTATUS(status) == (kind == 0 ? 0 : 42);
}
}  // namespace

int main() {
  for (const int signal : {SIGSEGV, SIGILL}) {
    for (int kind = 0; kind != 3; ++kind) {
      if (!Run(signal, kind)) {
        std::fprintf(stderr, "FAIL: signal %d, case %d\n", signal, kind);
        return 1;
      }
    }
  }
  std::puts("PASS: 6 cases; recovered faults, prior context, default fatal signals");
}
