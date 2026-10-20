#include <Arduino.h>
#include "app.h"
// The 32-level XML limit also bounds TinyXML2's recursive tree destruction.
// Hardware boundary tests include that path and require at least 4 KiB spare.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
void setup(){appSetup();}
void loop(){appLoop();}
