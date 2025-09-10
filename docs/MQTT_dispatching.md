# command management and dispatching


- derive class from ```CommandWithRegistry``` - that will provide it with the command registry functionality
- in initializing the instance of the class,  create a  ```ED_MQTT_dispatcher::ctrlCommand``` instance for each command which needs to be managed. Bind the class function executing the command by assigning ```funcPointer```
- add the command to the internal registry using registerCommand
- have the class subscribe its grabcommand function to the event managers of the trrigger class

**Expected rersult:**
command will be transparently dispatched and launched as needed whenerer the trigger class calls the grabcommand

# ctrlCommand format

a command is expected to have the following format

:<CMD> <default> -<t> <tparam>

CMD -> the executable command identifier. uppercase max 6 chars
default -> optional, default parameter. it will be made available among parameters with the key _default_
t -> a tag parameter, optional, which might specify a option or flag or used to specify a further input parameter (following value)

## example
:FMCO
firmware confirmation
:FMUP v1.0
firmware update to version v1.0
:PGFR 120 -L 3
set pingfrequency to 120 and pinmg logging level to 3