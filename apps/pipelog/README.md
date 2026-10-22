# PipeLog — a local multi-process log collector

PipeLog is an actual consumer of win-pipe: build workers and local tools send records to a central collector, which appends JSONL to a file and returns a request-specific application acknowledgement. It does not depend on JSON or networking libraries.

Build with tests/build.ps1. The executable is under .build/<toolchain-architecture-configuration>/pipelog.exe.

    pipelog.exe serve build-log "C:\logs\build.jsonl"
    pipelog.exe send build-log info "Compilation started"
    pipelog.exe send build-log warn "Retrying download" 2000

serve accepts an optional final positive record count for batch collection. Otherwise it runs until Ctrl+C. Output is appended; existing logs are preserved. Command arguments and output records use UTF-8 internally, with wide-character Windows command-line handling. The collector limits the native request to 64 KiB; send accepts message text up to 60000 UTF-8 bytes.

Each record has UTC time, a GUID request ID, level and message. Quotes, backslashes, newlines and control characters are JSON-escaped. Supported levels are info, warn and error.

RECORDED and exit 0 mean the collector appended and flushed its C++ output stream and the application acknowledgement reached the client. This is stronger than transport receipt, but does not promise physical-disk/power-loss durability. Exit 2 means application rejection; exit 3 means delivery is uncertain or no collector/acknowledgement was available. The tool does not automatically retry; request IDs make investigating an uncertain result possible. Invalid command arguments return 1.

The integration test starts twelve concurrent client processes, validates their application acknowledgements and parses the resulting file. It checks Unicode, multiline/quoted text, record uniqueness, missing collectors and invalid levels. This consumer validates a concrete local logging workload; it is not a general remote logging service or authenticated RPC system.

