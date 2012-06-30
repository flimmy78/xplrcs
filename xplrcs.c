/*
*    xplrcs - an RCS RS-485 thermostat to xPL bridge
*    Copyright (C) 2012  Stephen A. Rodgers
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*
*    xPL bridge to RCS RC65 thermostat
*
*
*/


#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <ctype.h>
#include <getopt.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <xPL.h>
#include "serio.h"
#include "notify.h"

#define SHORT_OPTIONS "a:d:hi:l:np:v"

#define WS_SIZE 256

#define POLL_RATE_CFG_NAME	"prate"
#define DEF_POLL_RATE		5
#define DEF_COM_PORT		"/dev/ttyS0"

/* 
* Command types
*/

typedef enum {CMDTYPE_TRANSP, CMDTYPE_BASIC} CmdType_t;


/*
* Command queueing structure
*/

typedef struct cmd_entry CmdEntry_t;

struct cmd_entry {
	String cmd;
	CmdType_t type;
	CmdEntry_t *prev;
	CmdEntry_t *next;
};

	


char *progName;
int debugLvl = 0; 
Bool noBackground = FALSE;
int xplrcsAddress = 1;
int pollRate = 5;
Bool pollPending = FALSE;
CmdEntry_t *cmdEntryHead = NULL;
CmdEntry_t *cmdEntryTail = NULL;



static seriostuff_t *serioStuff = NULL;
static xPL_ServicePtr xplrcsService = NULL;
static xPL_MessagePtr xplrcsStatusMessage = NULL;
static xPL_MessagePtr xplrcsTriggerMessage = NULL;
static char comPort[WS_SIZE] = DEF_COM_PORT;
static char interface[WS_SIZE] = "";
static char logPath[WS_SIZE] = "";
static char lastLine[WS_SIZE]; 

/* Commandline options. */

static struct option longOptions[] = {
  {"address", 1, 0, 'a'},
  {"com-port", 1, 0, 'p'},
  {"debug", 1, 0, 'd'},
  {"help", 0, 0, 'h'},
  {"interface", 1, 0, 'i'},
  {"log", 1, 0, 'l'},
  {"no-background", 0, 0, 'n'},
  {"version", 0, 0, 'v'},
  {0, 0, 0, 0}
};

/* Basic command list */

static const String const basicCommandList[] = {
	"hvac-mode",
	"fan-mode",
	NULL
};

/* Heating and cooling modes */

static const String const modeList[] = {
	"off",
	"heat",
	"cool",
	"auto",
	NULL
};

/* Commands for modes */

static const String const modeCommands[] = {
	" M=O",
	" M=H",
	" M=C",
	" M=A",
	NULL
};	

/* Fan modes  */

static const String const fanModeList[] = {
	"auto",
	"on",
	NULL
};

/* Commands for fan modes */

static const String const fanModeCommands[] = {
	" FM=0",
	" FM=1",
	NULL
};


/*
* Change string to lower case
* Warning: String must be nul terminated.
*/

static String str2Lower(char *q)
{
	char *p;
			
	if(q){
		for (p = q; *p; ++p) *p = tolower(*p);
	}
	return q;
}

/*
* Change string to upper case
* Warning: String must be nul terminated.
*/

static String str2Upper(char *q)
{
	char *p;
			
	if(q){
		for (p = q; *p; ++p) *p = toupper(*p);
	}
	return q;
}





/*
* Set a config integer value
*/

static void setConfigInt(xPL_ServicePtr theService, String theName, int theInt)
{
	char stringRep[18];
	sprintf(stringRep, "%d", theInt);
	xPL_setServiceConfigValue(theService, theName, stringRep);
}

/*
* Get a config integer value
*/

static int getConfigInt(xPL_ServicePtr theService, String theName)
{
	return atoi(xPL_getServiceConfigValue(theService, theName));
}


/*
* Parse config change request 
*/


static void parseConfig(xPL_ServicePtr theService)
{

	int newPRate = getConfigInt(theService, POLL_RATE_CFG_NAME);


	/* Handle bad configurable (override it) */
	if ((newPRate < 1) || newPRate > 60) {
		setConfigInt(theService, POLL_RATE_CFG_NAME, pollRate );
		return;
	}

	/* Install new poll rate */
	pollRate = newPRate;
}

/*
* Handle config change requests
*/

static void configChangedHandler(xPL_ServicePtr theService, xPL_ObjectPtr userData)
{
  	/* Read config items for service and install */
  	parseConfig(theService);
}

/*
* When the user hits ^C, logically shutdown
* (including telling the network the service is ending)
*/

static void shutdownHandler(int onSignal)
{
	xPL_setServiceEnabled(xplrcsService, FALSE);
	xPL_releaseService(xplrcsService);
	xPL_shutdown();
	exit(0);
}

/*
* Parse an RC65 status line into its constituant elements
*/

static int parseRC65Status(String ws, String *list, int limit)
{
	int i;
	String strtokrArgSave, arg, argList;
	const String argDelim = " ";

	/* Bail if pointers are NULL or work string has zero length */
	if(!ws  || !list || !strlen(ws))
		return 0;

	for(argList = ws, i = 0; (i < limit) && (arg = strtok_r(argList, argDelim, &strtokrArgSave)); argList = NULL){
		debug(DEBUG_ACTION,"Arg: %s", arg);
		list[i++] = arg;
	}
	list[i] = NULL; /* Terminate end of list */
	return i;
}

/*
* Queue a command entry
*/

static void queueCommand( String cmd, CmdType_t type )
{
	CmdEntry_t *newCE = malloc(sizeof(CmdEntry_t));
	/* Did malloc succeed ? */
	if(!newCE)
		fatal("malloc() failed in queueCommand");
	else
		memset(newCE, 0, sizeof(CmdEntry_t)); /* Zero it out */
	/* Dup the command string */
	newCE->cmd = strdup(cmd);

	if(!newCE->cmd) /* Did strdup succeed? */
		fatal("strdup() failed in queueCommand");

	/* Save the type */
	newCE->type = type;

	if(!cmdEntryHead){ /* Empty list */
		cmdEntryHead = cmdEntryTail =  newCE;
	}
	else{ /* List not empty */
		cmdEntryTail->next = newCE;
		newCE->prev = cmdEntryTail;
		cmdEntryTail = newCE;
	}

}

/*
* Dequeue a command entry
*/

static CmdEntry_t *dequeueCommand()
{
	CmdEntry_t *entry;

	if(!cmdEntryHead){
		entry = NULL;
	}
	else if(cmdEntryHead == cmdEntryTail){
		entry = cmdEntryHead;
		entry->prev = entry->next = cmdEntryHead = cmdEntryTail = NULL;
	}
	else{
		entry = cmdEntryHead;
		cmdEntryHead = cmdEntryHead->next;
		entry->prev = entry->next = cmdEntryHead->prev = NULL;	
	}
	return entry;

}

/*
* Free a command entry
*/

static void freeCommand( CmdEntry_t *e)
{
	if(e){
		if(e->cmd)
			free(e->cmd);
		free(e);
	}
}

/*
* Match a command from a NULL-terminated list, return index to list entry
*/

static int matchCommand(const String const *commandList, const String const command)
{
	int i;

	for(i = 0; commandList[i]; i++){
		/* debug(DEBUG_ACTION, "command = %s, commandList[%d] = %s", command, i, commandList[i]); */
		if(!strcmp(command, commandList[i]))
			break;
	}
	return i;	
}

/*
* Command hander for hvac-mode
*/

String doHVACMode(String ws, xPL_MessagePtr theMessage, const String const zone)
{
	String res = NULL;
	int i;
	const String const mode = xPL_getMessageNamedValue(theMessage, "mode");

	if(mode){
		i = matchCommand(modeList, mode);
		if(modeList[i]){
			res = strcat(ws, modeCommands[i]);
		}
	}
	return res;
}

/*
* Command handler for fan mode
*/

String doFanMode(String ws, xPL_MessagePtr theMessage, const String const zone)
{
	String res = NULL;
	int i;
	const String const mode = xPL_getMessageNamedValue(theMessage, "mode");

	if(mode){
		i = matchCommand(fanModeList, mode);
		if(fanModeList[i]){
			res = strcat(ws, fanModeCommands[i]);
		}
	}
	return res;
}



/*
* Our Listener 
*/



static void xPLListener(xPL_MessagePtr theMessage, xPL_ObjectPtr userValue)
{
	int i, nvCount, charsInBuffer;
	String p, ws, cmd = NULL;
	xPL_NameValueListPtr msgBody;

	

	if(!xPL_isBroadcastMessage(theMessage)){ /* If not a broadcast message */
		if(xPL_MESSAGE_COMMAND == xPL_getMessageType(theMessage)){ /* If the message is a command */
			const String const type = xPL_getSchemaType(theMessage);
			const String const class = xPL_getSchemaClass(theMessage);
			if(!(ws = malloc(WS_SIZE)))
				fatal("Cannot allocate work string in xPLListener");
			ws[0] = 0;
 
			debug(DEBUG_EXPECTED, "Command Received: Type = %s, Class = %s", type, class);
			if(!strcmp(class,"hvac")){
				if(!strcmp(type, "transp")){ /* Transparent schema */
					debug(DEBUG_ACTION, "We have a transparent command schema");

					/* Append the controller address */
					sprintf(ws, "A=%d ", xplrcsAddress);

					/* Get the message body */					
					if((msgBody = xPL_getMessageBody(theMessage))){

						nvCount = xPL_getNamedValueCount(msgBody);
						/* Iterate over name value pairs till they are all processed, or we run out of buffer */
						for(i = 0, charsInBuffer = strlen(ws); (charsInBuffer < WS_SIZE - 33) && (i < nvCount); i++){
							/* Get the name value pair */
							xPL_NameValuePairPtr nvpp = xPL_getNamedValuePairAt(msgBody, i);
							if(nvpp && !nvpp->isBinary){
								p = ws + strlen(ws); /* point to end of current string */
								charsInBuffer += snprintf(p, 33, "%s=%s", nvpp->itemName, nvpp->itemValue);
								strcat(ws," "); /* Add delimiter */
								charsInBuffer++;
							}
						}
						debug(DEBUG_ACTION, "Parsed xplrcs command: %s", ws);
						/* send the command */
						/* Add a return for the benefit of the RCS controller */
						queueCommand(ws, CMDTYPE_TRANSP);
					}
				}
				else if(!strcmp(type, "basic")){ /* Basic command schema */
					const String const command = xPL_getMessageNamedValue(theMessage, "command");
					if(command){
						const String const zone = xPL_getMessageNamedValue(theMessage, "zone");
						if(zone){ /* If valid zone */
							/* FIXME Map zone to address here */
							strcat(ws, "A=1");
							switch(matchCommand(basicCommandList, command)){
								case 0: /* hvac-mode */
									cmd = doHVACMode(ws, theMessage, zone);
									break;

								case 1: /* fan-mode */
									cmd = doFanMode(ws, theMessage, zone);
									break;
					
								default:
									debug(DEBUG_UNEXPECTED, "Unrecognized command: %s", command);
									break;
							}
						}
						if(cmd){
							queueCommand(cmd, CMDTYPE_BASIC); /* Queue the command */
						}

					}
					else{
						debug(DEBUG_UNEXPECTED, "No command key in message");
					}
				}
				else if(!strcmp(type, "request")){ /* Request command schema */
				}
			}
			free(ws);
		}

	}
}


/*
* Serial I/O handler (Callback from xPL)
*/

static void serioHandler(int fd, int revents, int userValue)
{
	int curArgc,lastArgc, sendAll = FALSE, i;
	String line;
	String pd,wscur,wslast,arg;
	String curArgList[20];
	String lastArgList[20];

	
	/* Do non-blocking line read */
	if(serio_nb_line_read(serioStuff)){
		/* Got a line */
		line = serio_line(serioStuff);
		if(pollPending){
			pollPending = FALSE;
			/* Has to be a response to a poll */
			/* Compare with last line received */
			if(strcmp(line, lastLine)){
				/* Clear any old name/values */
				xPL_clearMessageNamedValues(xplrcsTriggerMessage);
				debug(DEBUG_STATUS, "Got updated poll status: %s", line);

				/* Make working strings from the current and list lines */
				wscur = strdup(line);
				wslast = strdup(lastLine);
				if(!wscur || !wslast){
					fatal("Out of memory in serioHandler(): point 1");
				}

				/* Parse the current and last lists for comparison */
	
				curArgc = parseRC65Status(wscur, curArgList, 19);
				lastArgc = parseRC65Status(wslast, lastArgList, 19);

				/* If arg list mismatch, set the sendAll flag */
				if(lastArgc != curArgc){
					sendAll = TRUE;
				}

				/* Iterate through the list and figure out which args to send */
				for(i = 0; i < curArgc; i++){
					arg = curArgList[i];

					/* If sendAll is set or args changed, then add the arg to the list of things to send */
					if(sendAll || strcmp(curArgList[i], lastArgList[i])){
						if(!(pd = strchr(arg, '='))){
							debug(DEBUG_UNEXPECTED, "Parse error in %s point 1", arg);
							sendAll = TRUE;
							continue;
						}
						str2Lower(arg); /* Lower case arg */
						*pd = 0;
						pd++;
						if(strcmp(arg, "a")){ /* Do not send address arg */
							debug(DEBUG_EXPECTED, "Adding: key = %s, value = %s", arg, pd);
							/* Set key and value */
							xPL_setMessageNamedValue(xplrcsTriggerMessage, arg, pd);
						}
					}

				}
				if(!xPL_sendMessage(xplrcsTriggerMessage)){
					debug(DEBUG_UNEXPECTED, "Trigger message transmission failed");
				}
				/* Free working strings */
				free(wscur);
				free(wslast);

				/* Copy current string into last string for future comparisons */	
				strncpy(lastLine, line, WS_SIZE);
				lastLine[WS_SIZE - 1] = 0;
			}
		} /* End if(pollPending) */
		else{
			/* It's a response not related to a poll */
			if(!(wscur = strdup(line)))
				fatal("Out of memory in serioHandler(): point 2");
			debug(DEBUG_EXPECTED, "Non-poll response: %s", wscur);
			curArgc = parseRC65Status(wscur, curArgList, 19);
			xPL_clearMessageNamedValues(xplrcsStatusMessage);
			for(i = 0; i < curArgc; i++){
				arg = curArgList[i];
				str2Lower(arg); /* Lower case arg */
				if(!(pd = strchr(arg, '='))){
					debug(DEBUG_UNEXPECTED, "Parse error in %s point 2", arg);
					continue;
				}

				*pd = 0;
				pd++;
				if(strcmp(arg, "a")){  /* Do not send address arg */
					debug(DEBUG_EXPECTED, "Adding: key = %s, value = %s", arg, pd);
					/* Set key and value */
					xPL_setMessageNamedValue(xplrcsStatusMessage, arg, pd);
				}

			}
			if(!xPL_sendMessage(xplrcsStatusMessage))
				debug(DEBUG_UNEXPECTED, "Status message transmission failed");
			free(wscur); /* Free working string */
		}
	} /* End serio_nb_line_read */
}


/*
* Our tick handler. 
* This is used to synchonize the sending of data to the RCS thermostat.
*/

static void tickHandler(int userVal, xPL_ObjectPtr obj)
{
	CmdEntry_t *cmdEntry;
	static short pollCtr = 0;


	pollCtr++;

	debug(DEBUG_EXPECTED, "TICK: %d", pollCtr);
	/* Process clock tick update checking */

	if((cmdEntry = dequeueCommand())){ /* If command pending */
		/* Uppercase the command string */
		str2Upper(cmdEntry->cmd);
		debug(DEBUG_EXPECTED, "Sending command: %s", cmdEntry->cmd);
		serio_printf(serioStuff, "%s\r", cmdEntry->cmd);
		freeCommand(cmdEntry);
	}	
	else if(pollCtr >= pollRate){ /* Else check poll counter */
		pollCtr = 0;
		debug(DEBUG_ACTION, "Polling Status...");
		serio_printf(serioStuff, "A=%d R=1\r", xplrcsAddress);
		pollPending = TRUE;
	}	
}


/*
* Show help
*/

void showHelp(void)
{
	printf("'%s' is a daemon that bridges xPL to xplrcs thermostats\n", progName);
	printf("via an RS-232 or RS-485 interface\n");
	printf("\n");
	printf("Usage: %s [OPTION]...\n", progName);
	printf("\n");
	printf("  -a, --address ADDR      Set the address for the RC-65 thermostat\n");
	printf("                          (Valid addresses are 0 - 255, %d is the default)\n", xplrcsAddress); 
	printf("  -d, --debug LEVEL       Set the debug level, 0 is off, the\n");
	printf("                          compiled-in default is %d and the max\n", debugLvl);
	printf("                          level allowed is %d\n", DEBUG_MAX);
	printf("  -h, --help              Shows this\n");
	printf("  -i, --interface NAME    Set the broadcast interface (e.g. eth0)\n");
	printf("  -l, --log  PATH         Path name to log file when daemonized\n");
	printf("  -n, --no-background     Do not fork into the background (useful for debugging)\n");
	printf("  -p, --com-port PORT     Set the communications port (default is %s)\n", comPort);
	printf("  -v, --version           Display program version\n");
	printf("\n");
 	printf("Report bugs to <%s>\n\n", EMAIL);
	return;

}


/*
* main
*/


int main(int argc, char *argv[])
{
	int longindex;
	int optchar;

	/* Set the program name */
	progName=argv[0];

	/* Parse the arguments. */
	while((optchar=getopt_long(argc, argv, SHORT_OPTIONS, longOptions, &longindex)) != EOF) {
		
		/* Handle each argument. */
		switch(optchar) {
			
			/* Was it a long option? */
			case 0:
				
				/* Hrmm, something we don't know about? */
				fatal("Unhandled long getopt option '%s'", longOptions[longindex].name);
			
			/* If it was an error, exit right here. */
			case '?':
				exit(1);
		
			/* Was it a thermostat address? */
			case 'a':
				xplrcsAddress = atoi(optarg);
				if(xplrcsAddress < 0 || xplrcsAddress > 255) {
					fatal("Invalid thermostat address");
				}
				break;

        
			/* Was it a debug level set? */
			case 'd':

				/* Save the value. */
				debugLvl=atoi(optarg);
				if(debugLvl < 0 || debugLvl > DEBUG_MAX) {
					fatal("Invalid debug level");
				}

				break;
			
			/* Was it a help request? */
			case 'h':
				showHelp();
				exit(0);

			/* Specify interface to broadcast on */
			case 'i': 
				strncpy(interface, optarg, WS_SIZE -1);
				interface[WS_SIZE - 1] = 0;
				xPL_setBroadcastInterface(interface);
				break;

			case 'l':
				/* Override log path*/
				strncpy(logPath, optarg, WS_SIZE - 1);
				logPath[WS_SIZE - 1] = 0;
				debug(DEBUG_ACTION,"New log path is: %s",
				logPath);

				break;


			/* Was it a no-backgrounding request? */
			case 'n':

				/* Mark that we shouldn't background. */
				noBackground = TRUE;

				break;
			case 'p':
				/* Override com port*/
				strncpy(comPort, optarg, WS_SIZE - 1);
				comPort[WS_SIZE - 1] = 0;
				debug(DEBUG_ACTION,"New com port is: %s",
				comPort);

				break;


			/* Was it a version request? */
			case 'v':
				printf("Version: %s\n", VERSION);
				exit(0);
	

			
			/* It was something weird.. */
			default:
				fatal("Unhandled getopt return value %d", optchar);
		}
	}

	
	/* If there were any extra arguments, we should complain. */

	if(optind < argc) {
		fatal("Extra argument on commandline, '%s'", argv[optind]);
	}

	/* Turn on library debugging for level 5 */
	if(debugLvl >= 5)
		xPL_setDebugging(TRUE);

  
	/* Fork into the background. */

	if(!noBackground) {
		int retval;
		debug(DEBUG_STATUS, "Forking into background");

    		/* 
		* If debugging is enabled, and we are daemonized, redirect the debug output to a log file if
    		* the path to the logfile is defined
		*/

		if((debugLvl) && (logPath[0]))                          
			notify_logpath(logPath);

		/* Fork and exit the parent */

		if((retval = fork())){
      			if(retval > 0)
				exit(0);  /* Exit parent */
			else
				fatal_with_reason(errno, "parent fork");
    		}



		/*
		* The child creates a new session leader
		* This divorces us from the controlling TTY
		*/

		if(setsid() == -1)
			fatal_with_reason(errno, "creating session leader with setsid");


		/*
		* Fork and exit the session leader, this prohibits
		* reattachment of a controlling TTY.
		*/

		if((retval = fork())){
			if(retval > 0)
        			exit(0); /* exit session leader */
			else
				fatal_with_reason(errno, "session leader fork");
		}

		/* 
		* Change to the root of all file systems to
		* prevent mount/unmount problems.
		*/

		if(chdir("/"))
			fatal_with_reason(errno, "chdir to /");

		/* set the desired umask bits */

		umask(022);
		
		/* Close STDIN, STDOUT, and STDERR */

		close(0);
		close(1);
		close(2);
		} 

	/* Start xPL up */
	if (!xPL_initialize(xPL_getParsedConnectionType())) {
		fatal("Unable to start xPL lib");
	}

	/* Initialze xplrcs service */

	/* Create a configurable service and set our application version */
	xplrcsService = xPL_createConfigurableService("hwstar", "xplrcs", "xplrcs.xpl");
  	xPL_setServiceVersion(xplrcsService, VERSION);

	/* If the configuration was not reloaded, then this is our first time and   */
	/* we need to define what the configurables are and what the default values */
 	/* should be.                                                               */
	if (!xPL_isServiceConfigured(xplrcsService)) {
  		/* Define a configurable item and give it a default */
		xPL_addServiceConfigurable(xplrcsService, POLL_RATE_CFG_NAME, xPL_CONFIG_RECONF, 1);

		setConfigInt(xplrcsService, POLL_RATE_CFG_NAME, DEF_POLL_RATE);
  	}

  	/* Parse the service configurables into a form this program */
  	/* can use (whether we read a config or not)                */
  	parseConfig(xplrcsService);

 	/* Add a service change listener we'll use to pick up a new tick rate */
 	xPL_addServiceConfigChangedListener(xplrcsService, configChangedHandler, NULL);

	/*
	* Create a status message object
	*/

  	xplrcsStatusMessage = xPL_createBroadcastMessage(xplrcsService, xPL_MESSAGE_STATUS);
  	xPL_setSchema(xplrcsStatusMessage, "xplrcs", "status");

	/*
	* Create a trigger message object
	*/

	xplrcsTriggerMessage = xPL_createBroadcastMessage(xplrcsService, xPL_MESSAGE_TRIGGER);
  	xPL_setSchema(xplrcsTriggerMessage, "xplrcs", "trigger");


  	/* Install signal traps for proper shutdown */
 	signal(SIGTERM, shutdownHandler);
 	signal(SIGINT, shutdownHandler);

	/* Initialize the COM port */
	
	if(!(serioStuff = serio_open(comPort, 9600)))
		fatal("Could not open com port: %s", comPort);


	/* Flush any partial commands */
	serio_printf(serioStuff, "\r");
	usleep(100000);
	serio_flush_input(serioStuff);


 	/* Enable the service */
  	xPL_setServiceEnabled(xplrcsService, TRUE);

	/* Ask xPL to monitor our serial device */
	if(xPL_addIODevice(serioHandler, 1234, serio_fd(serioStuff), TRUE, FALSE, FALSE) == FALSE)
		fatal("Could not register serial I/O fd with xPL");

	/* Add 1 second tick service */
	xPL_addTimeoutHandler(tickHandler, 1, NULL);

  	/* And a listener for all xPL messages */
  	xPL_addMessageListener(xPLListener, NULL);


 	/** Main Loop **/

	for (;;) {
		/* Let XPL run forever */
		xPL_processMessages(-1);
  	}

	exit(1);
}
