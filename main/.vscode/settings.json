{
    "//": "Devin-generated: clangd-based IntelliSense voor ESP-IDF + esp-matter. Microsoft C/C++ extension's IntelliSense uitschakelen om dubbele diagnostics te voorkomen.",
    "C_Cpp.intelliSenseEngine": "disabled",
    "C_Cpp.autocomplete": "disabled",
    "C_Cpp.errorSquiggles": "disabled",

    "clangd.path": "clangd",
    "clangd.arguments": [
        "--compile-commands-dir=${workspaceFolder}/build",
        "--background-index",
        "--clang-tidy",
        "--header-insertion=never",
        "--completion-style=detailed",
        "--query-driver=**/riscv32-esp-elf-gcc,**/riscv32-esp-elf-g++,**/xtensa-esp32*-elf-gcc,**/xtensa-esp32*-elf-g++"
    ],
    "clangd.onConfigChanged": "restart",
    "clangd.detectExtensionConflicts": true,

    "files.associations": {
        "*.h": "c",
        "*.cpp": "cpp",
        "Kconfig*": "kconfig"
    },

    "files.exclude": {
        "**/build": true,
        "**/.cache": true,
        "**/managed_components": false
    },

    "editor.formatOnSave": false,
    "search.exclude": {
        "**/build": true,
        "**/managed_components": true
    }
}
