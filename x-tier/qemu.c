#include "qemu.h"
#include "debug.h"
#include "inject.h"
#include "external_command.h"

#include "../include/sysemu/kvm.h"
#include "../include/sysemu/sysemu.h"

#include "../include/exec/cpu-all.h"
#include "../include/exec/cpu-common.h"

#include "../include/exec/memory.h"
#include "../include/exec/address-spaces.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>

/*
 * Questions
 */
#define XTIER_QUESTION_OBTAIN_FILE_NAME 2
#define XTIER_QUESTION_OS 8
#define XTIER_QUESTION_INJECT_GET_FILE 9
#define XTIER_QUESTION_EVENT_INJECT_SELECT_MODULE 10


/*
 * TIME
 */
#define NSEC_PER_SEC 1000000000L
#define NSEC_PER_MSEC 1000000L

/*
 * TYPEDEFS
 */
// Ask a questions. Returns 1 on valid answer and -X on error.
typedef int (*XTIER_question_callback)(const char *cmdline);
typedef void (*XTIER_command_callback)(const char *cmdline);

/*
 * Structures
 */
struct XTIER_time {
	size_t sec;
	size_t ms;
	size_t ns;
};

struct XTIER_question;

struct XTIER_choice {
	int choice;                           // The number representing this choice.
	const char *description;              // The description of the choice (Printed if help is requested).
	struct XTIER_question *sub_question;  // A Pointer to a sub question that may follow this choice.
};

struct XTIER_question {
	int id;                               // should be one of the question ids.
	XTIER_question_callback callback;     // The function that will handle the user input.
	struct XTIER_choice *choices;         // The different choices for the question.

	int number_of_choices;
};

struct XTIER_command {
	const char *name;                       // The name of the command, this string will be matched against the user input.
	const char *description;                // The description of the command
	XTIER_command_callback callback;        // The callback that will be used.
	struct XTIER_command *sub_commands;     // The list of subcommands
	int number_of_sub_commands;             // The number of subcommands
};


// The current XTIER config.
struct xtier_config _XTIER;

CPUState *cpu_state = NULL;

int _initialized = 0;

MemoryRegion *inject_memory;

int _event_injection = 0;
uint32_t _auto_injection = 0;
uint32_t _time_injection = 0;
struct injection *_injection = 0;

struct XTIER_external_command _external_command;
struct XTIER_external_command_redirect _external_command_redirect;
int _external_command_fd = 0;

/*
 * Questions and Choices
 *
 * Update these structures to provide new questions and choices for
 * a user.
 */
struct XTIER_question *_current_question;
XTIER_command_callback _return_to;


struct XTIER_choice os_choices[] = {
	{
		.choice = XTIER_OS_LINUX_64,
		.description = "GNU/Linux 64-bit",
		.sub_question = NULL,
	},
	{
		.choice = XTIER_OS_WINDOWS_7_32,
		.description = "Windows 7 32-bit",
		.sub_question = NULL,
	},
	{
		.choice = XTIER_OS_LINUX_32,
		.description = "GNU/Linux 32-bit",
		.sub_question = NULL,
	},
};

struct XTIER_choice XTIER_event_inject[] = {
};

/*
 * Commands
 *
 * Update these structures to add new commands.
 */
struct XTIER_command *_current_command;
struct XTIER_command *_current_command_parent;

struct XTIER_command top_level_commands[] = {
	{
		.name = "cont",
		.description = "Resume VM and return to 'monitor' Mode.",
		.callback = XTIER_switch_to_monitor_mode,
		.sub_commands = NULL,
		.number_of_sub_commands = 0,
	},
	{
		.name = "monitor",
		.description = "Return to 'monitor' Mode, but do not resume VM.",
		.callback = XTIER_switch_to_monitor_mode_keep_paused,
		.sub_commands = NULL,
		.number_of_sub_commands = 0,
	},
	{
		.name = "quit",
		.description = "Return to 'monitor' Mode. VM will be paused.",
		.callback = XTIER_switch_to_monitor_mode_keep_paused,
		.sub_commands = NULL,
		.number_of_sub_commands = 0,
	},
	{
		.name = "exit",
		.description = "Return to 'monitor' Mode. VM will be paused.",
		.callback = XTIER_switch_to_monitor_mode_keep_paused,
		.sub_commands = NULL,
		.number_of_sub_commands = 0,
	},
	{
		.name = "help",
		.description = "Print help information.",
		.callback = XTIER_print_help,
		.sub_commands = NULL,
		.number_of_sub_commands = 0,
	},
	{
		.name = "external",
		.description = "Receive an external command.",
		.callback = XTIER_command_receive_external_command,
		.sub_commands = NULL,
		.number_of_sub_commands = 0,
	},
};


/*
 * Functions
 */
static void _XTIER_init(void)
{
	// Init State
	_XTIER.mode = 0;
	_XTIER.os = XTIER_OS_UNKNOWN;

	// Allocate memory within the guest
	inject_memory = g_malloc(sizeof(*inject_memory));

	printf("[inject memory in-guest] = %p\n", inject_memory);

	if (!inject_memory)
		PRINT_ERROR("Could not allocate memory");

	// Fixed size (1024 * 4096) for now.
	memory_region_init_ram(inject_memory,
	                       NULL,
	                       "X-TIER.data",
	                       XTIER_MEMORY_AREA_SIZE);

	// Fixed offset (1 UL << 30) and priority (1337) for now
	memory_region_add_subregion_overlap(get_system_memory(),
	                                    XTIER_MEMORY_AREA_ADDRESS,
	                                    inject_memory,
	                                    1337);

	_initialized = 1;
}

int XTIER_ioctl(unsigned int command, void *arg)
{
	return kvm_vcpu_ioctl(CPU(X86_CPU(cpu_state)), command, arg);
}

static struct XTIER_command * _find_command(const char *name)
{
	int i;
	int size;
	struct XTIER_command *cmd;

	if(_current_command)
	{
		if(!_current_command->sub_commands)
			return NULL;

		size = _current_command->number_of_sub_commands;
		cmd = _current_command->sub_commands;
	}
	else
	{
		size = ARRAY_SIZE(top_level_commands);
		cmd = top_level_commands;
	}


	for(i = 0; i < size; i++)
	{
		if(strcmp(cmd[i].name, name) == 0)
			return &cmd[i];
	}

	return NULL;
}

static struct XTIER_choice * _find_choice(struct XTIER_question *q, int choice)
{
	int i;

	for(i = 0; i < q->number_of_choices; i++)
	{
		if (q->choices[i].choice == choice)
			return &q->choices[i];
	}

	return NULL;
}

/*
 * Print a long line of data to the console.
 *
 * @param padding_left The space that will be left free on each line that the data occupies
 * @param width The width of a single line
 * @param data The pointer to the data to be printed.
 */
static void _print_long_line(int padding_left, int width, const char *data)
{
	int distance = 0;
	int printed = 0;
	char data_copy[strlen(data)];
	char *next;
	int space_left = width;

	// Copy data to be able to manipulate it
	strcpy(data_copy, data);
	next = strtok(data_copy, " \n");

	while(next)
	{
		// If the word is longer than width, we abort
		distance = strlen(next);
		if(distance > width)
		{
			PRINT_WARNING("The given data contains a word that is too long for the specified width!\n");
			return;
		}

		// Does the word fit in the current line?
		if(space_left - distance >= 0)
		{
			printed += distance;

			if(data[printed] == ' ')
			{
				PRINT_OUTPUT("%s ", next);
				space_left -= (distance + 1);
			}
			else
			{
				PRINT_OUTPUT("%s", next);
				PRINT_OUTPUT("\n%*.*s", padding_left, padding_left, "");
				space_left = width;
			}

			// Space / \n
			printed++;

			// Find next word
			next = strtok(NULL, " \n");
		}
		else
		{
			// Print new line
			PRINT_OUTPUT("\n%*.*s", padding_left, padding_left, "");
			space_left = width;
		}
	}

	PRINT_OUTPUT("\n");
}

/*
 * Print the question that is currently stored in the _current_question
 * variable.
 */
void XTIER_ask_current_question(void)
{
	if(!_current_question) {
		PRINT_WARNING("There is currently no question set!\n");
		return;
	}

	// Add new questions here
	switch(_current_question->id)
	{
	case XTIER_QUESTION_OBTAIN_FILE_NAME:
		PRINT_OUTPUT("\nPlease enter the name of the file where to save to.\n"
		             "Existing files will be overwritten!\n");
		break;
	case XTIER_QUESTION_OS:
		PRINT_OUTPUT("\nPlease specify the guest OS.\nType 'help' to see the available choices.\n");
		break;
	case XTIER_QUESTION_INJECT_GET_FILE:
		PRINT_OUTPUT("\nPlease specify the binary file that contains the code that will be injected.\n");
		break;
	case XTIER_QUESTION_EVENT_INJECT_SELECT_MODULE:
		PRINT_OUTPUT("\nPlease select the event based module that you want to inject.\n");
		break;
	default:
		PRINT_WARNING("Unknown question id!\n");
		break;
	}
}


/*
 * Generic function that prints help for questions or commands.
 */
void XTIER_print_help(const char *cmdline)
{
	int size;
	int i;

	// Is help requested for the current question?
	if(_current_question)
	{
		if(!_current_question->choices)
		{
			PRINT_ERROR("This question has no options!\n");
			return;
		}

		// Print Options
		size = _current_question->number_of_choices;

		PRINT_OUTPUT("\n Available Options:\n");

		// Header
		PRINT_OUTPUT("%5.5s%-10.10s%5.5s%-40.40s\n", "", "CHOICE", "", "MEANING");
		PRINT_OUTPUT("%5.5s%-10.10s%5.5s%-40.40s\n", "", "------", "",
		             "--------------------------------------------------------------------------------------");

		for(i = 0; i < size; i++)
		{
			PRINT_OUTPUT("%5.5s%-10d%5.5s", "", _current_question->choices[i].choice, "");

			_print_long_line(20, 40, _current_question->choices[i].description);
		}
	}
	// Print all subcommands if a certain command was selected
	else if(_current_command_parent && _current_command_parent->sub_commands)
	{
		size = _current_command_parent->number_of_sub_commands;

		PRINT_OUTPUT("\n Sub Commands of '%s':\n\n", _current_command_parent->name);

		// Header
		PRINT_OUTPUT("%5.5s%-20.20s%5.5s%-40.40s\n", "", "SUB COMMANDS", "", "DESCRIPTION");
		PRINT_OUTPUT("%5.5s%-20.20s%5.5s%-40.40s\n", "", "------------", "",
		             "--------------------------------------------------------------------------------------");


		for(i = 0; i < size; i++)
		{
			PRINT_OUTPUT("%5.5s%-20.20s%5.5s", "", _current_command_parent->sub_commands[i].name,"");
			_print_long_line(30, 40, _current_command_parent->sub_commands[i].description);
		}

	}
	else
	{
		// Print available commands
		PRINT_OUTPUT("\n Available Commands:\n");

		// Header
		PRINT_OUTPUT("%5.5s%-20.20s%5.5s%-40.40s\n", "", "COMMANDS", "", "DESCRIPTION");
		PRINT_OUTPUT("%5.5s%-20.20s%5.5s%-40.40s\n", "", "--------", "",
		             "--------------------------------------------------------------------------------------");

		size = ARRAY_SIZE(top_level_commands);

		for(i = 0; i < size; i++)
		{
			PRINT_OUTPUT("%5.5s%-20.20s%5.5s", "", top_level_commands[i].name,"");
			_print_long_line(30, 40, top_level_commands[i].description);
		}
	}

	PRINT_OUTPUT("\n");
}

/*
 * Generic function that handles all XTIER input.
 */
void XTIER_handle_input(Monitor *mon, const char *cmdline, void *opaque)
{
	char cmdline_copy[strlen(cmdline)];
	char *cmd_part = NULL;

	int ret = 0;
	int choice = 0;
	int cmdline_pointer = 0;
	struct XTIER_choice *ch = NULL;
	struct XTIER_command *cmd = NULL;

	// Copy cmdline
	strcpy(cmdline_copy, cmdline);
	cmd_part = strtok(cmdline_copy, " ");

	// Do we process a question or a commmand?
	// Lets first check if we can find the command
	while(cmd_part)
	{
		cmd = _find_command(cmd_part);

		if(cmd)
		{
			_current_command_parent = _current_command;
			_current_command = cmd;
			cmdline_pointer += strlen(cmd_part) + 1;
		}
		else
			break;

		cmd_part = strtok(NULL, " ");
	}

	if(_current_command)
	{
		// BINGO
		if(_current_command->callback)
		{
			_current_command->callback((cmdline + cmdline_pointer));
		}
		else
		{
			PRINT_ERROR("Specified command is invalid without options!\n");
			XTIER_print_help(cmdline);
		}

		_current_command = NULL;
		_current_command_parent = NULL;
	}
	// Question?
	else if(_current_question)
	{
		// Handle choice
		choice = atoi(cmdline);
		ret = _current_question->callback(cmdline);

		if(ret < 0)
		{
			_current_question = NULL;
		}
		else
		{
			// Update current question if possible.
			ch = _find_choice(_current_question, choice);
			if(ch && ch->sub_question)
			{
				// Set Question
				_current_question = ch->sub_question;
				// Ask Question
				if(_current_question)
					XTIER_ask_current_question();
			}
			else if(_return_to)
			{
				// The new command may specify a question
				_return_to = NULL;
			}
			else
			{
				_current_question = NULL;
			}
		}
	}
	else
	{
		PRINT_ERROR("Unknown Command: %s\n", cmdline);
	}

	if(strcmp(cmdline, "cont") && strcmp(cmdline, "quit") && strcmp(cmdline, "exit")) {
		PRINT_OUTPUT(XTIER_PROMPT);
	}
}

/*
 * Enter XTIER input mode.
 * The machine will be paused and the XTIER shell will be enabled.
 */
void XTIER_switch_to_XTIER_mode(CPUState *env)
{
	PRINT_OUTPUT("\tSwitching to 'XTIER' Mode...\n\tThe VM will be stopped...\n");

	vm_stop(RUN_STATE_PAUSED);

	// set env
	cpu_state = env;

	// Init if necessary
	if (!_initialized) {
		_XTIER_init();
	}

	// Print the current question if any
	if (_current_question) {
		XTIER_ask_current_question();
		PRINT_OUTPUT(XTIER_PROMPT);
	}

	// Start "shell" ;)
	XTIER_start_getting_user_input(XTIER_handle_input);
}


/*
 * Return to normal input mode.
 */
void XTIER_switch_to_monitor_mode(const char *cmdline)
{
	PRINT_OUTPUT("\tSwitching to 'monitor' Mode...\n\tThe VM will be started...\n");

	XTIER_stop_getting_user_input();

	// Reset
	_current_question = NULL;
	_current_command = NULL;

	// Continue
	vm_start();
}

/*
 * Return to normal input mode. However the VM will remain paused.
 */
void XTIER_switch_to_monitor_mode_keep_paused(const char *cmdline)
{
	PRINT_OUTPUT("\tSwitching to 'monitor' Mode...\n");

	XTIER_stop_getting_user_input();

	// Reset
	_current_question = NULL;
	_current_command = NULL;
}

void XTIER_command_receive_external_command(const char *cmdline)
{
	int ret = 0;
	int n = 0, m = 0;

	// Create named pipe - User Read, Write, Exec
	if (!_external_command_fd) {
		if (mkfifo(injection_input_pipe_filename, S_IRWXU) == 0) {
			PRINT_INFO("created cmd external->x-tier fifo %s\n", injection_input_pipe_filename);
		}
		else {
			if (errno != EEXIST) {
				PRINT_ERROR("Could not create named pipe '%s'!\n", injection_input_pipe_filename);
				return;
			} else {
				PRINT_INFO("cmd external->x-tier pipe %s already present\n", injection_input_pipe_filename);
			}
		}
	}

	// Open the fd
	if (!_external_command_fd) {
		if ((_external_command_fd = open(injection_input_pipe_filename, O_RDONLY)) < 0) {
			PRINT_ERROR("Could not open fd to named pipe '%s'!\n", injection_input_pipe_filename);
			return;
		}
	}

	PRINT_INFO("Opened named pipe '%s' for reading...\n", injection_input_pipe_filename);
	PRINT_INFO("Waiting for external command struct... Process will be blocked!\n");

	n = read(_external_command_fd, &_external_command, sizeof(struct XTIER_external_command));
	m = sizeof(struct XTIER_external_command) - n;

	if (m != 0) {
		PRINT_ERROR("Wrong size when receiving the command struct: size off %d, read %d. Aborting...\n", m, n);
		return;
	}

	PRINT_DEBUG("Received command structure...\n");

	// Output redirection?
	if (_external_command.redirect != NONE) {

		// Get the redirection struct
		if (read(_external_command_fd, &_external_command_redirect, sizeof(struct XTIER_external_command_redirect))
		    != sizeof(struct XTIER_external_command_redirect)) {
			PRINT_ERROR("Wrong size reading the redirect struct. Aborting...\n");
			return;
		}

		PRINT_DEBUG("Received redirect structure...\n");

		// Make sure the data redirection steam is NULL
		_external_command_redirect.stream = NULL;

		// Is this a pipe redirection?
		if (_external_command_redirect.type == PIPE) {
			_external_command_redirect.stream = fopen(_external_command_redirect.filename, "w");

			if (_external_command_redirect.stream <= 0) {
				PRINT_ERROR("Could not open file '%s'\n", _external_command_redirect.filename);
				return;
			}

			// We just opened the file, so we send the data begin header
			fprintf(_external_command_redirect.stream, "" XTIER_EXTERNAL_OUTPUT_BEGIN);
		} else {
			PRINT_DEBUG("unhandled output redirection type requested.");
		}
	} else {
		//no output redirection requested
	}

	// Get the command itself
	if (_external_command.type == INJECTION) {
		// Injection command
		// Free old injection structure if there is any
		if (_injection) {
			free_injection_without_code(_injection);
		}

		// Make sure the OS is set
		if(_XTIER.os == XTIER_OS_UNKNOWN) {
			_XTIER.os = XTIER_OS_LINUX_64;
			XTIER_ioctl(XTIER_IOCTL_SET_XTIER_STATE, &_XTIER);
		}

		// Get the injection structure
		_injection = injection_from_fd(_external_command_fd);

		// Read in data if required
		if (_injection->code_len == 0) {
			PRINT_ERROR("Received injection doesn't have code!\n");
			return;
		}

		// Inject
		PRINT_DEBUG("Injecting file %s...\n", _injection->name);
		PRINT_DEBUG("|_ consists of %d bytes code\n", _injection->code_len);
		PRINT_DEBUG("|_ consists of %d arguments of overall size %u\n", _injection->argc, _injection->args_size);
		print_injection(_injection);

		PRINT_DEBUG("ioctl for injection NOW!\n");
		ret = XTIER_ioctl(XTIER_IOCTL_INJECT, _injection);
		if (ret < 0) {
			PRINT_ERROR("error occurred while injecting the file: %d\n", ret);
		}
		else {
			PRINT_DEBUG("returned %d from injection ioctl\n", ret);
		}

		// Synchronize new cpu_state
		cpu_state->kvm_vcpu_dirty = 0; // Force the sync
		XTIER_synchronize_state(cpu_state);

		// fprintf(stderr, "RIP now: 0x%llx\n", cpu_state->eip);
	}
	else {
		// Unknown command
		PRINT_ERROR("Unkown command type (%d)\n", _external_command.type);
		return;
	}

	//keep the fd open for now.
	//close(_external_command_fd);
	//_external_command_fd = 0;
}

static void XTIER_ns_to_time(size_t ns, struct XTIER_time *time)
{
	if(!time)
		return;

	time->sec = ns / NSEC_PER_SEC;
	ns = ns - (time->sec * NSEC_PER_SEC);
	time->ms = ns / NSEC_PER_MSEC;
	time->ns = ns % NSEC_PER_MSEC;

	return;
}

/*
 * Prints statistics about timing and others of the injection
 */
static struct xtier_stats XTIER_print_injection_performance(void)
{
	int ret;
	struct XTIER_time t;
	struct xtier_stats perf;

	// Get performacne data
	ret = XTIER_ioctl(XTIER_IOCTL_INJECT_GET_PERFORMANCE, &perf);

	if (ret < 0) {
		PRINT_ERROR("An error occurred while obtaining the performance data of the injection!\n");
		return perf;
	}

	// Print statistics
	PRINT_OUTPUT("\n\nInjection Statistics:\n");
	PRINT_OUTPUT("\t | File: '%s'\n", _injection->name);
	PRINT_OUTPUT("\t | Injections: %u\n", perf.injections);
	PRINT_OUTPUT("\t | Temp Removals/Resumes: %u\n", perf.temp_removals);
	PRINT_OUTPUT("\t | Hypercalls: %u\n", perf.hypercalls);

	if (perf.injections) {
		XTIER_ns_to_time(perf.total_module_load_time / perf.injections, &t);
		PRINT_OUTPUT("\t | Average Load Time: %zu s %zu ms %zu ns\n",
		             t.sec, t.ms, t.ns);
		XTIER_ns_to_time(perf.total_module_exec_time / perf.injections, &t);
		PRINT_OUTPUT("\t | Average Exec Time: %zu s %zu ms %zu ns\n",
		             t.sec, t.ms, t.ns);
		XTIER_ns_to_time(perf.total_module_unload_time / perf.injections, &t);
		PRINT_OUTPUT("\t | Average Unload Time: %zu s %zu ms %zu ns\n",
		             t.sec, t.ms, t.ns);

		PRINT_OUTPUT("\t |\n");

		if (perf.hypercalls) {
			XTIER_ns_to_time(perf.total_module_hypercall_time / perf.hypercalls, &t);
			PRINT_OUTPUT("\t | Average Hypercall Time: %zu s %zu ms %zu ns\n", t.sec, t.ms, t.ns);

			XTIER_ns_to_time(perf.total_module_hypercall_time / perf.injections, &t);
			PRINT_OUTPUT("\t | Average Hypercall Time per Injection: %zu s %zu ms %zu ns\n", t.sec, t.ms, t.ns);

			XTIER_ns_to_time(perf.total_module_hypercall_time, &t);
			PRINT_OUTPUT("\t | Total Hypercall Time: %zu s %zu ms %zu ns\n", t.sec, t.ms, t.ns);

			PRINT_OUTPUT("\t |\n");
		}

		if (perf.temp_removals) {
			XTIER_ns_to_time(perf.total_module_temp_removal_time / perf.temp_removals, &t);
			PRINT_OUTPUT("\t | Average Temp Removal Time: %zu s %zu ms %zu ns\n", t.sec, t.ms, t.ns);

			XTIER_ns_to_time(perf.total_module_temp_resume_time / perf.temp_removals, &t);
			PRINT_OUTPUT("\t | Average Temp Resume Time: %zu s %zu ms %zu ns\n", t.sec, t.ms, t.ns);

			XTIER_ns_to_time((perf.total_module_temp_removal_time + perf.total_module_temp_resume_time) / perf.temp_removals, &t);
			PRINT_OUTPUT("\t | Average Temp Removal/Resume Time: %zu s %zu ms %zu ns\n", t.sec, t.ms, t.ns);

			XTIER_ns_to_time((perf.total_module_temp_removal_time + perf.total_module_temp_resume_time) / perf.injections, &t);
			PRINT_OUTPUT("\t | Average Temp Removal/Resume Time per Injection: %zu s %zu ms %zu ns\n", t.sec, t.ms, t.ns);

			XTIER_ns_to_time((perf.total_module_temp_removal_time + perf.total_module_temp_resume_time), &t);
			PRINT_OUTPUT("\t | Total Temp Removal/Resume Time: %zu s %zu ms %zu ns\n", t.sec, t.ms, t.ns);

			PRINT_OUTPUT("\t |\n");
		}

		XTIER_ns_to_time((perf.total_module_load_time +
		                  perf.total_module_exec_time +
		                  perf.total_module_unload_time)
		                 / perf.injections, &t);
		PRINT_OUTPUT("\t | Average Total Time: %zu s %zu ms %zu ns\n", t.sec, t.ms, t.ns);

		if (perf.temp_removals) {
			XTIER_ns_to_time((perf.total_module_exec_time -
			                  (perf.total_module_temp_removal_time + perf.total_module_temp_resume_time))
			                 / perf.injections, &t);
			PRINT_OUTPUT("\t | Average Exec Time w/o TEMP Removal/Resume: %zu s %zu ms %zu ns\n", t.sec, t.ms, t.ns);
		}

		if (perf.hypercalls) {
			XTIER_ns_to_time((perf.total_module_exec_time - perf.total_module_hypercall_time) / perf.injections, &t);
			PRINT_OUTPUT("\t | Average Exec Time w/o Hypercalls: %zu s %zu ms %zu ns\n", t.sec, t.ms, t.ns);
		}

		if (perf.temp_removals && perf.hypercalls) {
			XTIER_ns_to_time((perf.total_module_exec_time -
			                  (perf.total_module_temp_removal_time + perf.total_module_temp_resume_time + perf.total_module_hypercall_time))
			                 / perf.injections, &t);
			PRINT_OUTPUT("\t | Average Exec Time w/o TEMP R/R & Hypercalls: %zu s %zu ms %zu ns\n", t.sec, t.ms, t.ns);
		}

		XTIER_ns_to_time((perf.total_module_load_time +
		                  perf.total_module_exec_time +
		                  perf.total_module_unload_time), &t);
		PRINT_OUTPUT("\t | Total Time: %zu s %zu ms %zu ns\n", t.sec, t.ms, t.ns);
	}

	PRINT_OUTPUT("\t ___________________________________\n\n");

	return perf;
}

static void XTIER_handle_injection_finished(void)
{
	// Print performance data
	XTIER_print_injection_performance();

	struct xtier_state state;
	int ret;

	// fetch injection state, return value etc
	if ((ret = XTIER_ioctl(XTIER_IOCTL_INJECT_GET_STATE, &state)) < 0) {
		PRINT_ERROR("An error occurred while obtaining the performance data of the injection!\n");
		return;
	}

	int64_t return_value = state.return_value;

	PRINT_OUTPUT("Injection finished (return value %ld)!\n", return_value);
	PRINT_INFO("Injection finished (return value %ld)!\n", return_value);
	PRINT_INFO("Injection CR3: %zx!\n", state.cr3);

	// Notify waiting applications if any
	if (_external_command_redirect.type != NONE && _external_command_redirect.stream) {
		// Send Return Value
		XTIER_external_command_send_return_value(_external_command_redirect.stream, return_value);

		// Send end Notifier
		fprintf(_external_command_redirect.stream, "" XTIER_EXTERNAL_OUTPUT_END);

		// Close and reset
		fclose(_external_command_redirect.stream);

		_external_command_redirect.type = NONE;
		_external_command_redirect.stream = NULL;
	}

	// Sync
	XTIER_synchronize_state(cpu_state);
}

static void XTIER_handle_injection_fault(void)
{
	PRINT_ERROR("An exception occurred during the injection that could not be handled!\n");

	// Sync
	XTIER_synchronize_state(cpu_state);
}


void XTIER_synchronize_state(CPUState *state)
{
	cpu_synchronize_state(state);
}

/*
 * Handle kvm exits due to XTIER.
 */
int XTIER_handle_exit(CPUState *env, uint64_t exit_reason)
{
	PRINT_DEBUG("Handling kvm EXIT: %zx...\n", exit_reason);

	int ret = 0;

	switch(exit_reason) {
	case XTIER_EXIT_REASON_INJECT_FINISHED:
		XTIER_handle_injection_finished();
		XTIER_switch_to_XTIER_mode(env);
		PRINT_OUTPUT(XTIER_PROMPT);
		break;

	case XTIER_EXIT_REASON_INJECT_FAULT:
		XTIER_handle_injection_fault();
		XTIER_switch_to_XTIER_mode(env);
		PRINT_OUTPUT(XTIER_PROMPT);
		break;

	case XTIER_EXIT_REASON_INJECT_COMMAND:
		// sync state
		XTIER_synchronize_state(env);

		// handle
		XTIER_inject_handle_interrupt(env, &_external_command_redirect);
		break;

	case XTIER_EXIT_REASON_DEBUG:
		PRINT_INFO("Debug exit requested.\n");
		XTIER_switch_to_XTIER_mode(env);
		PRINT_OUTPUT(XTIER_PROMPT);
		break;

	default:
		PRINT_ERROR("Unknown exit reason!\n");
	}

	return ret;
}
