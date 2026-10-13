# Command-line parser

`command_line.h` exposes the small public boundary used by the executable:

```cpp
const dfa::cli::ParseResult parsed = dfa::cli::Parse(argc, argv);
```

The parser accepts either the implicit `set-default` action or an explicit
`set-default` token.  `check` can be written as a command token or as
`--check`.  Long option aliases with underscores are retained for compatibility
with the original tool (`--exe_path`, `--icon_idx`, `--icon_path`, and
`--prog_name`).

Paths are received as UTF-16 from `wmain`, parsed by CLI11's wide-character
entry point and converted back with `CLI::widen`,
and returned as `std::filesystem::path`/`std::wstring`.  This keeps non-ASCII
Windows paths intact for the rest of the application.
