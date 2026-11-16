// Turns LeakSanitizer off at build time for every fuzz and -standalone binary (ASan and UBSan stay
// fully on). Leaks are not the bug class these targets look for.
extern "C" int __lsan_is_turned_off() {
  return 1;
}
