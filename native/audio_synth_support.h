#pragma once

namespace music::detail {
// FluidSynth is compiled without its threading support. All audio objects in a
// process therefore share the first calling OS thread; use processes for parallelism.
void initialize_fluid_runtime();
}
