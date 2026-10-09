// OS loading policy for the CLI's explicit --extension arguments.
#ifndef LHAT_CLI_NATIVE_EXTENSIONS_H
#define LHAT_CLI_NATIVE_EXTENSIONS_H
#include <lhat/extension.h>
bool cli_extension_path(const char *path);
bool cli_extensions_register(LhatProgram *program);
void cli_extensions_dispose(void);
#endif
