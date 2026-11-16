#pragma once
#include <stdbool.h>

bool torch_supported(void);
bool torch_available(void);
bool torch_enabled(void);
bool torch_set(bool enabled);
