#include <zephyr.h>
#include <misc/printk.h>
#include <misc/util.h>
#include <device.h>
#include <board.h>
#include <pinmux.h>
#include <pwm.h>
#include <gpio.h>
#include <x86intrin.h>
#include <shell/shell.h>
#include <version.h>


#define GPIO_DRIVER CONFIG_GPIO_DW_0_NAME
#define PIN 3
#define IN 2
#define OUT 6
#define NUM 500
#define STACKSIZE 1024
#define PRIORITY 5
#define LOW_PRIO 10
#define CYCLE 1/50 // about 25 Hz - 1/50 down and 1/50 UP

/*
 * There are multiple threads doing printfs and they may conflict.
 * Therefore use puts() instead of printf().
 */
#if defined(CONFIG_STDOUT_CONSOLE)
#define PRINTF(...) { char output[256]; \
		      sprintf(output, __VA_ARGS__); puts(output); }
#else
#define PRINTF(...) printk(__VA_ARGS__)
#endif

uint64_t start, finish, latency;	// for our timestamps

int index1 = 0;
uint64_t measure1[NUM] = {0};		// buffer for latency with no background

int index2 = 0;
uint64_t measure2[NUM] = {0};		// buffer for latency with background

uint64_t callOnly = 0;			// measures overhead of syscall only 
uint64_t callNswitch = 0;		// measures overhead of syscall and a context switch
uint64_t measure3[NUM] = {0};		// buffer for only context switch time = sysSwitch - sysOnly

int numMeasure = 0; 			//to keep track of which array to populate for which measurement
int condition = 1; 			//to let the message passign threads know when to stop and exit
int condition1 = 1; 			//to let the slave thread know when to stop

//Declare some semaphores to do alternation between threads
K_SEM_DEFINE(S0, 1, 1);			/* starts off "available" */
K_SEM_DEFINE(S1, 0, 1);			/* starts off "not available" */

// Define a message structure for message passing
struct data_item_type {
	u32_t thing1;
	u32_t thing2;
	u32_t thing3;
	u32_t thing4;
	u32_t thing5;
	u32_t thing6;
	u32_t thing7;
	u32_t thing8;
	u32_t thing9;
};

K_MSGQ_DEFINE(my_msgq, sizeof(struct data_item_type), 1000, 4); // message que of 1000 entries

static struct gpio_callback gpio_cb;

// callback function for when we trigger interrupt
void inputRcv(struct device *gpiob, struct gpio_callback *cb,
		    u32_t pins)
{
	finish = __rdtsc();
	latency = finish - start;
	//printk("%llu - %llu\n", finish, start);
	if(numMeasure == 1)
	{
		measure1[index1] = latency;
		//printk("Entry1 %d = %llu\n", index1, measure1[index1]);
		index1++;
	}
	else //nunMeasure == 2
	{
		measure2[index2] = latency;
		//printk("Entry2 %d = %llu\n", index2, measure2[index2]);
		index2++;
	}
	//printk("Button pressed at %d\n", k_cycle_get_32());
}

// busy loop to simulate doing computations
void doStuff()
{
	int j = 0;
	for(int i = 0; i < 100; i++)
	{
		j = j + i; 
	}
}

// Message sender for measurement 2
void producer_thread(void *dummy1, void *dummy2, void *dummy3)
{
	ARG_UNUSED(dummy1);
	ARG_UNUSED(dummy2);
	ARG_UNUSED(dummy3);
	struct data_item_type data;

	while (condition) {
		/* create data item to send - in this case jsut dummy data we dont care */
		data.thing1 = 1;
		data.thing2 = 2;
		data.thing3 = 3;
		data.thing4 = 4;
		data.thing5 = 5;
		data.thing6 = 6;
		data.thing7 = 7;
		data.thing8 = 8;
		data.thing9 = 9;

		/* send data to consumers */
		while (k_msgq_put(&my_msgq, &data, K_NO_WAIT) != 0) {
			    /* message queue is full: purge old data & try again */
			    //k_msgq_purge(&my_msgq);
			    //printk("PURGING!\n");
		}
		//k_sleep(CYCLE/1000);
		/* data item was successfully added to message queue */
	}
}

K_THREAD_STACK_DEFINE(producer_thread_stack_area, STACKSIZE);
static struct k_thread producer_thread_data;

// Message receiver for measurement 2
void consumer_thread(void *dummy1, void *dummy2, void *dummy3)
{
	ARG_UNUSED(dummy1);
	ARG_UNUSED(dummy2);
	ARG_UNUSED(dummy3);
	struct data_item_type data;
	u32_t result;

	while (condition) {
		/* get a data item */
		k_msgq_get(&my_msgq, &data, K_FOREVER);
		/* process data item  - in this case jsut dummy processing*/
		result = data.thing1 + data.thing2 + data.thing3;
		//printk("Passing around msg %d\n", result);
	}
}

K_THREAD_STACK_DEFINE(consumer_thread_stack_area, STACKSIZE);
static struct k_thread consumer_thread_data;

// Low prio thread for measuring context switches
void slave_thread(void *dummy1, void *dummy2, void *dummy3)
{
	ARG_UNUSED(dummy1);
	ARG_UNUSED(dummy2);
	ARG_UNUSED(dummy3);
	k_sleep(1);
	while(condition1)
	{
		k_sem_take(&S0, K_FOREVER); // P(S0) 
		doStuff();
		start = __rdtsc(); // From here *** in low prio thread we get context switched to high prio thread main
		k_sem_give(&S1); // V(S1) - singal the other guy
	}
	//exit
}

K_THREAD_STACK_DEFINE(slave_thread_stack_area, STACKSIZE);
static struct k_thread slave_thread_data;

/***********************************************************
 Define some commands that my shell will be able to process
************************************************************/

// Cammand to print out the first measuremnts
static int shell_cmd_latency(int argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	printk("Printing latencies with no background computing:\n");
	for(int i = 0; i < NUM; i++)
	{
		printk("%llu\n", measure1[i]);
	}
	return 0;
}

// Command to print out the second measuremnts
static int shell_cmd_latencyBusy(int argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	printk("Printing latencies with background computing:\n");
	for(int i = 0; i < NUM; i++)
	{
		printk("%llu\n", measure2[i]);
	}
	return 0;
}

// Cammand to print out the third measuremnts
static int shell_cmd_contextSwitch(int argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	printk("Printing context switching overhead:\n");
	for(int i = 0; i < NUM; i++)
	{
		printk("%llu\n", measure3[i]);
	}
	return 0;
}

#define SHELL_CMD_VERSION "version"
static int shell_cmd_version(int argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	printk("Zephyr version %s\n", KERNEL_VERSION_STRING);
	return 0;
}

SHELL_REGISTER_COMMAND(SHELL_CMD_VERSION, shell_cmd_version,
		       "Show kernel version");

#define MY_SHELL_MODULE "measure" // name of the shell to use

// Let shell know that these are the things it can handle for me
static struct shell_cmd commands[] = {
	{ "latency", shell_cmd_latency, "Print latencies with no background computing." },
	{ "latencyBusy", shell_cmd_latencyBusy, "Print latencies with background computing." },
	{ "contextSwitch", shell_cmd_contextSwitch, "Print context switching overhead." },
	{ NULL, NULL, NULL }
};


void main(void)
{
	struct device *pin_mux;
	struct device *gpiob;
	u32_t pin = PIN;

	gpiob = device_get_binding(GPIO_DRIVER); // get binding for gpio_dw driver
	if (!gpiob) {
		printk("error\n");
		return;
	}
	
	gpio_pin_configure(gpiob, IN, GPIO_DIR_IN | GPIO_INT |  GPIO_PUD_NORMAL | GPIO_INT_EDGE | GPIO_INT_ACTIVE_HIGH); //configure pin 2 (IO10) to be input interrupt pin with detection on rising edge

	//gpio_pin_configure(gpiob, OUT, GPIO_DIR_OUT); //configure pin 0 (IO8) to be output

	
	// Now configure the callback routine for when we trigger and interrupt on rising edge
	gpio_init_callback(&gpio_cb, inputRcv, BIT(IN));
	gpio_add_callback(gpiob, &gpio_cb);
	gpio_pin_enable_callback(gpiob, IN);

	
	pin_mux = device_get_binding(CONFIG_PINMUX_NAME); // get binding for pinmux driver
	if (!pin_mux) 
	{
		printk("Cannot find %s!\n", CONFIG_PINMUX_NAME);
		return;
	}
	
	pinmux_pin_set(pin_mux, pin, PINMUX_FUNC_A); // set IO3 to be GPIO6 OUT

	//pinmux_pin_set(pin_mux, UART, PINMUX_FUNC_C); // set pin 3 to PWM.LED1 <- PWM
	/*
	pwm_dev = device_get_binding(PWM_DRIVER); //get binding for pwm driver
	if (!pwm_dev) 
	{
		printk("Cannot find %s!\n", PWM_DRIVER);
		return;
	}
	
	pwm_pin_set_cycles(pwm_dev, pwm_pin, period, pulse); // run PWM with 50% duty cycle
	*/
	gpio_pin_write(gpiob, OUT, 0); //initialize to 0
	numMeasure = 1;

	for(int i = 0; i < NUM; i++)// take 500 measurements for latency
	{ 
		//u32_t val = 0;
		//gpio_pin_read(gpiob, OUT, &val);
		//printk("OUT = %d\t", val);
		//gpio_pin_read(gpiob, IN, &val);
		//printk("IN = %d\n", val);
		k_sleep(CYCLE);
		start = __rdtsc();
		gpio_pin_write(gpiob, OUT, 1);
		//gpio_pin_read(gpiob, OUT, &val);
		//printk("OUT = %d\t", val);
		//gpio_pin_read(gpiob, IN, &val);
		//printk("IN = %d\n", val);
		k_sleep(CYCLE);
		gpio_pin_write(gpiob, OUT, 0);

	}

	condition = 1; //set condition

	//Now set up some background computing to do the measurements for busy latency
	k_thread_create(&consumer_thread_data, consumer_thread_stack_area, STACKSIZE,
			consumer_thread, NULL, NULL, NULL,
			PRIORITY, 0, K_NO_WAIT);

	k_thread_create(&producer_thread_data, producer_thread_stack_area, STACKSIZE,
			producer_thread, NULL, NULL, NULL,
			PRIORITY, 0, K_NO_WAIT);

	//From now on these two threads will be running in the background
	// SO now do another 500 measurements for busy latency
	numMeasure = 2;
	gpio_pin_write(gpiob, OUT, 0); //initialize to 0
	
	for(int i = 0; i < NUM; i++)
	{ 
		//u32_t val = 0;
		//gpio_pin_read(gpiob, OUT, &val);
		//printk("OUT = %d\t", val);
		//gpio_pin_read(gpiob, IN, &val);
		//printk("IN = %d\n", val);
		k_sleep(CYCLE);
		start = __rdtsc();
		gpio_pin_write(gpiob, OUT, 1);
		//gpio_pin_read(gpiob, OUT, &val);
		//printk("OUT = %d\t", val);
		//gpio_pin_read(gpiob, IN, &val);
		//printk("IN = %d\n", val);
		k_sleep(CYCLE);
		gpio_pin_write(gpiob, OUT, 0);
	}
	
	condition = 0;	// done with measurement two, stop the background threads	
	
	k_sleep(2); //wait a bit

	// Finally, measure context switching, I use main, which has the highest priority of 0, and a slave thread with prio 10
	condition1 = 1;
	k_thread_create(&slave_thread_data, slave_thread_stack_area, STACKSIZE,
			slave_thread, NULL, NULL, NULL,
			LOW_PRIO, 0, K_NO_WAIT);
	
	// gather our 500 measurements
	for(int i = 0; i < NUM; i++)
	{ 
		k_sem_take(&S1, K_FOREVER); // P(S1) , which starts off unavailable so slave will have to go first
		finish = __rdtsc(); // *** context switched to here
		callNswitch = finish - start; // this will be latency of a switch and sys call
		doStuff();
		start = __rdtsc(); // From here -->
		k_sem_give(&S0); // V(S0) singal other guy
		finish = __rdtsc(); // <-- to here we can measure the time for just the system call
		callOnly = finish - start; // this will be latency of just a sys call
		//printk("CallNSwitch %llu - CallOnly %llu\n", callNswitch, callOnly);
		measure3[i] = callNswitch - callOnly; // context switch = (syscall and context switch) - (just syscall)
		//doStuff();
	}

	condition1 = 0;
	
	printk("\nMeasurements done! You may now issue shell commands!\n");

	SHELL_REGISTER(MY_SHELL_MODULE, commands);
}
