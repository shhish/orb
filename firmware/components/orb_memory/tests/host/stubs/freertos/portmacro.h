#pragma once
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0u
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
