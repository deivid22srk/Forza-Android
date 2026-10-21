// Hooks for default.xbe. A strong definition replaces the recompiled function
// at that address, for example:
//
// #include "xdk/hle.h"
//
// X86_EXTERN_FUNC(__imp__sub_00268EB5);
// X86_FUNC(sub_00268EB5) {
//     __imp__sub_00268EB5(ctx, base);  // the original
// }
//
// Instruction-level fixes belong in config/game.toml as [[hook]] entries, with
// the function defined here. Never edit generated/.
