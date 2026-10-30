#pragma once

/**
 * @brief Create the local-static initialization lock once before global constructors and
 * scheduler startup.
 */
void cxx_runtime_init(void);
