/* user_main.c -- ESP8266 MQTT battery monitor
*
* Configuration parameters set with the Makefile using a Python patching
* utility which is avalable on my github site. This allows the configurations
* to differ between nodes and also protects the WIFI login credentials by
* removing them from the source.
*
* Copyright (C) 2015, Stephen Rodgers <steve at rodgers 619 dot com>
* Copyright (c) 2014-2015, Tuan PM <tuanpm at live dot com>
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 
* Redistributions of source code must retain the above copyright notice,
* this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
* notice, this list of conditions and the following disclaimer in the
* documentation and/or other materials provided with the distribution.
* Neither the name of Redis nor the names of its contributors may be used
* to endorse or promote products derived from this software without
* specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*/

// API includes
#include "ets_sys.h"
#include "osapi.h"
#include "debug.h"
#include "gpio.h"
#include "user_interface.h"
#include "mem.h"
#include "jsonparse.h"
// Project includes
#include "driver/uart.h"
#include "mqtt.h"
#include "wifi.h"
#include "easygpio.h"
#include "util.h"
#include "kvstore.h"


/* General definitions */

#define ON 1
#define OFF 0

#define WIFI_LED 14						// WIFI LED GPIO


#define MAX_INFO_ELEMENTS 16			// Patcher number of elements
#define INFO_BLOCK_MAGIC 0x3F2A6C17		// Patcher magic
#define INFO_BLOCK_SIG "ESP8266HWSTARSR"// Patcher pattern
#define CONFIG_FLD_REQD 0x01			// Patcher field required flag


#define I2C_ADDR_BYTE(x,y) ((x) << 1) | ((y) & 1)
#define I2C_READ 1
#define I2C_WRITE 0


// INA226 I2C address 
#define INA226_ADDR 0x45


// INA226 register pointers 
#define INA226_CONFIG   0x00
#define INA226_SHUNT    0x01
#define INA226_BUS      0x02
#define INA226_POWER    0x03
#define INA226_CURRENT  0x04
#define INA226_CAL      0x05

// INA226 Initial Constants 
#define INA226_INIT_CONFIG 0x0927

// Misc constants 
#define VOLTRES 1250       // Microvolt per bit
#define VMAG -6
#define CMAG -7
#define PMAG -3


// Definition for a patcher config element

struct config_info_element_tag{
	uint8_t flags;
	uint8_t key[15];
	uint8_t value[80];
}  __attribute__((__packed__));

typedef struct config_info_element_tag config_info_element;

// Definition for a patcher config element

struct config_info_block_tag{
	uint8_t signature[16];
	uint32_t magic;
	uint8_t numelements;
	uint8_t recordLength;
	uint8_t pad[10];
	config_info_element e[MAX_INFO_ELEMENTS];
}  __attribute__((__packed__));

// Definition of a common element for MQTT command parameters

typedef union {
	char *sp;
	unsigned u;
	int i;
} pu;

// Definition of an MQTT command element

typedef struct {
	const char *command;
	uint8_t type;
	pu p;
} command_element;


typedef struct {
	uint32_t d1;
	uint8_t state;
	uint8_t command;
	uint8_t length;
	uint8_t buffer[24];
} i2c_t;

typedef struct config_info_block_tag config_info_block;

// I2C states
enum {I2CS_STARTUP = 0};

// Definition of command codes and types

enum {WIFISSID=0, WIFIPASS, MQTTHOST, MQTTPORT, MQTTSECUR, MQTTDEVID, 
	MQTTUSER, MQTTPASS, MQTTKPALIV, MQTTDEVPATH, MQTTBTLOCAL};
enum {CP_NONE= 0, CP_INT, CP_BOOL, CP_QSTRING};
 
/* Local function signatures */

LOCAL i2c_handler(void *arg); 
 
 
/* Local storage */

// Patcher configuration information


LOCAL config_info_block configInfoBlock = {
	.signature = INFO_BLOCK_SIG,
	.magic = INFO_BLOCK_MAGIC,
	.numelements = MAX_INFO_ELEMENTS,
	.recordLength = sizeof(config_info_element),
	.e[WIFISSID] = {.flags = CONFIG_FLD_REQD, .key = "WIFISSID", .value="your_ssid_here"},
	.e[WIFIPASS] = {.flags = CONFIG_FLD_REQD, .key = "WIFIPASS", .value="its_a_secret"},
	.e[MQTTHOST] = {.flags = CONFIG_FLD_REQD, .key = "MQTTHOST", .value="your_mqtt_broker_hostname_here"}, // May also be an IP address
	.e[MQTTPORT] = {.key = "MQTTPORT", .value="1883"}, // destination Port for mqtt broker
	.e[MQTTSECUR] = {.key = "MQTTSECUR",.value="0"}, // Security 0 - no encryption
	.e[MQTTDEVID] = {.key = "MQTTDEVID", .value="your_mqtt_device_id_here"}, // Unique device ID
	.e[MQTTUSER] = {.key = "MQTTUSER", .value="your_mqtt_client_name_here"}, // MQTT User name
	.e[MQTTPASS] = {.key = "MQTTPASS", .value="its_a_secret"},// MQTT Password
	.e[MQTTKPALIV] = {.key = "MQTTKPALIV", .value="120"}, // Keepalive interval
	.e[MQTTDEVPATH] = {.flags = CONFIG_FLD_REQD, .key = "MQTTDEVPATH", .value = "/home/lab/relay"}, // Device path
	.e[MQTTBTLOCAL] = {.key = "MQTTBTLOCAL", .value = "1"} // Optional local toggle control using GPIO0
};

// Command elements 
// Additional commands are added here
 
enum {CMD_OFF = 0, CMD_ON, CMD_TOGGLE, CMD_PULSE, CMD_BTLOCAL, CMD_QUERY, CMD_SURVEY, CMD_SSID, CMD_RESTART, CMD_WIFIPASS, CMD_CYCLE};

LOCAL command_element commandElements[] = {
	{.command = "off", .type = CP_NONE},
	{.command = "on", .type = CP_NONE},
	{.command = "toggle", .type = CP_NONE},
	{.command = "pulse", .type = CP_INT},
	{.command = "btlocal", .type = CP_INT},
	{.command = "query", .type = CP_NONE},
	{.command = "survey", .type = CP_NONE},
	{.command = "ssid", .type = CP_QSTRING},
	{.command = "restart",.type = CP_NONE},
	{.command = "wifipass",.type = CP_QSTRING},
	{.command = "cycle",.type = CP_INT},
	{.command = ""} /* End marker */
};
	
// Misc Local variables 

LOCAL char *commandTopic, *statusTopic;
LOCAL char *controlTopic = "/node/control";
LOCAL char *infoTopic = "/node/info";
LOCAL flash_handle_s *configHandle;
LOCAL os_timer_t mainTimer, i2cTimer;
LOCAL i2c_t i2c;

MQTT_Client mqttClient;			// Control block used by MQTT functions



/**
 * Publish connection info
 */
LOCAL void ICACHE_FLASH_ATTR publishConnInfo(MQTT_Client *client)
{
	struct ip_info ipConfig;
	char *buf = util_zalloc(256);	
		
	// Publish who we are and where we live
	wifi_get_ip_info(STATION_IF, &ipConfig);
	os_sprintf(buf, "{\"muster\":{\"connstate\":\"online\",\"device\":\"%s\",\"ip4\":\"%d.%d.%d.%d\",\"schema\":\"hwstar_battnode\",\"ssid\":\"%s\"}}",
			configInfoBlock.e[MQTTDEVPATH].value,
			*((uint8_t *) &ipConfig.ip.addr),
			*((uint8_t *) &ipConfig.ip.addr + 1),
			*((uint8_t *) &ipConfig.ip.addr + 2),
			*((uint8_t *) &ipConfig.ip.addr + 3),
			commandElements[CMD_SSID].p.sp);

	INFO("MQTT Node info: %s\r\n", buf);

	// Publish
	MQTT_Publish(client, infoTopic, buf, os_strlen(buf), 0, 0);
	
	// Free the buffer
	util_free(buf);
	
}



/**
 * Handle qstring command
 */
 
LOCAL void ICACHE_FLASH_ATTR handleQstringCommand(char *new_value, command_element *ce)
{
	char *buf = util_zalloc(128);
	
	
	if(!new_value){
		const char *cur_value = kvstore_get_string(configHandle, ce->command);
		os_sprintf(buf, "{\"%s\":\"%s\"}", ce->command, cur_value);
		util_free(cur_value);
		INFO("Query Result: %s\r\n", buf );
		MQTT_Publish(&mqttClient, statusTopic, buf, os_strlen(buf), 0, 0);
	}
	else{
		util_free(ce->p.sp); // Free old value
		ce->p.sp = new_value; // Save reference to new value
		kvstore_put(configHandle, ce->command, ce->p.sp);
		
	}

	util_free(buf);

}

/**
 * Send MQTT message to update relay state
 */
 
LOCAL void ICACHE_FLASH_ATTR updateRelayState(int s)
{
	char *state = s ? "\"on\"}" : "\"off\"}";
	char result[16];
	os_strcpy(result,"{\"relaystate\":");
	os_strcat(result, state);
	INFO("MQTT: New Relay State: %s\r\n", state);
	MQTT_Publish(&mqttClient, statusTopic, result, os_strlen(result), 0, 0);

}

/**
 * WIFI connect call back
 */
 

LOCAL void ICACHE_FLASH_ATTR wifiConnectCb(uint8_t status)
{
	if(status == STATION_GOT_IP){
		MQTT_Connect(&mqttClient);
	}

	else
		GPIO_OUTPUT_SET(WIFI_LED, TRUE); // LED off
}

/**
 * Survey complete,
 * publish results
 */


LOCAL void ICACHE_FLASH_ATTR
surveyCompleteCb(void *arg, STATUS status)
{
	struct bss_info *bss = arg;
	
	#define SURVEY_CHUNK_SIZE 256
	
	if(status == OK){
		uint8_t i;
		char *buf = util_zalloc(SURVEY_CHUNK_SIZE);
		bss = bss->next.stqe_next; //ignore first
		for(i = 2; (bss); i++){
			if(2 == i)
				os_sprintf(strlen(buf) + buf,"{\"access_points\":[");
			else
				os_strcat(buf,",");
			os_sprintf(strlen(buf)+ buf, "\"%s\":{\"chan\":\"%d\",\"rssi\":\"%d\"}", bss->ssid, bss->channel, bss->rssi);
			bss = bss->next.stqe_next;
			buf = util_str_realloc(buf, i * SURVEY_CHUNK_SIZE); // Grow buffer
		}
		if(buf[0])
			os_strcat(buf,"]}");
		
		INFO("Survey Results:\r\n", buf);
		INFO(buf);
		MQTT_Publish(&mqttClient, statusTopic, buf, os_strlen(buf), 0, 0);
		util_free(buf);
	}

}


/**
 * MQTT Connect call back
 */
 
LOCAL void ICACHE_FLASH_ATTR mqttConnectedCb(uint32_t *args)
{
	
	MQTT_Client* client = (MQTT_Client*)args;


	
	INFO("MQTT: Connected\r\n");
	GPIO_OUTPUT_SET(WIFI_LED, FALSE); // LED on
	
	
	

	publishConnInfo(client);
	
	// Subscribe to the control topic
	MQTT_Subscribe(client, controlTopic, 0);
	// Subscribe to command topic
	MQTT_Subscribe(client, commandTopic, 0);

}

/**
 * MQTT Disconnect call back
 */
 

LOCAL void ICACHE_FLASH_ATTR mqttDisconnectedCb(uint32_t *args)
{
	MQTT_Client* client = (MQTT_Client*)args;
	INFO("MQTT: Disconnected\r\n");
	GPIO_OUTPUT_SET(WIFI_LED, TRUE); // LED off
}

/**
 * MQTT published call back
 */

LOCAL void ICACHE_FLASH_ATTR mqttPublishedCb(uint32_t *args)
{
	MQTT_Client* client = (MQTT_Client*)args;
	INFO("MQTT: Published\r\n");
}

/**
 * MQTT Data call back
 * Commands are decoded and acted upon here
 */

LOCAL void ICACHE_FLASH_ATTR 
mqttDataCb(uint32_t *args, const char* topic, uint32_t topic_len, 
const char *data, uint32_t data_len)
{
	char *topicBuf, *dataBuf;
	uint8_t i;
	struct jsonparse_state state;
	char command[32];

	MQTT_Client* client = (MQTT_Client*)args; // Pointer to MQTT control block passed in as args
	
	command[0] = 0; // Zero command string length to prevent stack junk from printing during debug

	// Save local copies of the topic and data
	topicBuf = util_strndup(topic, topic_len);
	dataBuf = util_strndup(data, data_len);
	
	INFO("Receive topic: %s, data: %s \r\n", topicBuf, dataBuf);
	
	// Control Message?
	if(!os_strcmp(topicBuf, controlTopic)){
		jsonparse_setup(&state, dataBuf, data_len);
		if (util_parse_json_param(&state, "control", command, sizeof(command)) != 2)
			return; /* Command not present in json object */
		if(!os_strcmp(command, "muster")){
			publishConnInfo(&mqttClient);
		}
	}
	
	// Command Message?
	else if (!os_strcmp(topicBuf, commandTopic)){ // Check for match to command topic
		// Parse command
		jsonparse_setup(&state, dataBuf, data_len);
		if (util_parse_json_param(&state, "command", command, sizeof(command)) != 2)
			return; /* Command not present in json object */
				
		for(i = 0; commandElements[i].command[0]; i++){
			command_element *ce = &commandElements[i];
			//INFO("Trying %s\r\n", ce->command);
			if(CP_NONE == ce->type){ // Parameterless command
				if(!os_strcmp(command, ce->command)){
					switch(i){
						case CMD_OFF:
							break;
					
						case CMD_ON:
							break;
					
						case CMD_TOGGLE:
							break;
						
						case CMD_QUERY:
							break;
							
						case CMD_SURVEY:
							wifi_station_scan(NULL, surveyCompleteCb);
							break;
							
						case CMD_RESTART:
							util_restart();
							break;
							
						default:
							util_assert(FALSE, "Unsupported command: %d", i);
					}
					break;
				}
			}
			
			if((CP_INT == ce->type) || (CP_BOOL == ce->type)){ // Integer/bool parameter
				int arg;
				if(util_parse_command_int(command, ce->command, dataBuf, &arg)){
					switch(i){
						case CMD_PULSE: // Pulse rely on then off for a specific time in mSec
							break;
			
						case CMD_BTLOCAL: // Link or break button control from relay
							ce->p.i = (arg) ? 1: 0;
							kvstore_update_number(configHandle, ce->command, ce->p.i);
							break;
							
						case CMD_CYCLE: // Cycle relay
							if(arg && (arg < 500))
								arg = 500; // Clip to 1/2 sec half cycle
							arg = arg/100; // Convert 1ms to 100ms count
							ce->p.i = arg;
							if(!arg) // If paramter is 0, this is a request to stop cycling.
							break;
							
						default:
							util_assert(FALSE, "Unsupported command: %d", i);
						}
					break;
				}				
			}
			if(CP_QSTRING == ce->type){ // Query strings
				char *val = NULL;
				if(util_parse_command_qstring(command, ce->command, dataBuf, &val) != FALSE){
					if((CMD_SSID == i) || (CMD_WIFIPASS == i)){ // SSID or WIFIPASS?
						handleQstringCommand(val, ce);
					}
				}
			}
			
		} /* END for */
		kvstore_flush(configHandle); // Flush any changes back to the kvs
	} /* END if topic test */
				
	// Free local copies of the topic and data strings
	util_free(topicBuf);
	util_free(dataBuf);
}

// Read bytes from an I2C device

LOCAL void ICACHE_FLASH_ATTR  i2c_read_bytes(uint8_t len)
{
	uint8_t i;
	for(i = 0; i < len; i++){
		i2c.buffer[i] = i2c_master_readByte();
		if(i == len - 1)
			i2c_master_send_nack();
		else
			i2c_master_send_ack();
	}
}


// I2c one shot timer arm

LOCAL void ICACHE_FLASH_ATTR i2c_timer_arm( uint16_t delay)
{
	os_timer_disarm(&i2cTimer);
	os_timer_setfn(&i2cTimer, (os_timer_func_t *)i2c_handler, (void *)0);
	os_timer_arm(&i2cTimer, delay, 0);
}


/*
 * Place unsigned integer into buffer in big endian format
 */
LOCAL void ICACHE_FLASH_ATTR u16_to_bebytes(uint16_t val, uint8_t *bytes)
{
	bytes[0] = (uint8_t) (val >> 8);
	bytes[1] = (uint8_t) (val & 0xFF);
}

/*
 * Extract a big endian unsigned integer from a buffer
 */
  
LOCAL uint16_t ICACHE_FLASH_ATTR bebytes_to_u16(uint8_t *bytes)
{
	int val = ((uint16_t) bytes[0]) << 8;
	val += bytes[1];
	return val;
}

/*
 * Generic I2C read 
 */

LOCAL int ICACHE_FLASH_ATTR  i2c_generic_read(uint8_t addr, uint8_t length)
{
	i2c.length = length;
	
	// Start the transaction
	
	i2c_master_start();
	
	// Send the address
	
	i2c_master_writeByte(I2C_ADDR_BYTE(addr, I2C_READ));
	if(!i2c_master_checkAck()){
		i2c_master_stop();
		return FALSE;
	}
	// Read the bytes
	
	i2c_read_bytes(length);
	
	// End the transaction
	
	i2c_master_stop();
	return TRUE;
}

/*
 * Write something to an I2C device
 */

LOCAL int ICACHE_FLASH_ATTR i2c_write(uint8_t command, uint8_t addr, uint16_t wait_time, uint8_t *buffer, uint8_t length)
{
	int index = 0;
	// Start the transaction
	
	i2c_master_start();
	
	// Send the address
	
	i2c_master_writeByte(I2C_ADDR_BYTE(addr, I2C_WRITE));
	if(!i2c_master_checkAck()){
		i2c_master_stop();
		//INFO("err request 1\n");
		return FALSE;
	}	
	
	// Send the command
		
	i2c_master_writeByte(command);
	if(!i2c_master_checkAck()){
		i2c_master_stop();
		//INFO("err request 2\n");
		return FALSE;
	}
	
	// Optionally send data
	
	while(length){
		i2c_master_writeByte(buffer[index++]);
		if(!i2c_master_checkAck()){
			i2c_master_stop();
			return FALSE;
		}
		length--;
	}	
	
	// End the transaction
	
	i2c_master_stop();
	
	// Optionally, arm the timer
	
	if(wait_time)
		i2c_timer_arm(wait_time);
	return TRUE;
}

/*
 * Simplified i2c request function. (Address + command only)
 */
 
LOCAL int ICACHE_FLASH_ATTR i2c_request(uint8_t command, uint8_t addr, uint16_t wait_time)
{
	return i2c_write(command, addr, wait_time, NULL, 0);
}


// I2C handler

LOCAL int ICACHE_FLASH_ATTR  i2c_handler(void *arg)
{
	switch(i2c.state){
		case I2CS_STARTUP:
			// Request a config from the INA226
			i2c_request(INA226_CONFIG, INA226_ADDR, 0);
			
			i2c_generic_read(INA226_ADDR, 2);
			uint16_t val = bebytes_to_u16(i2c.buffer);
			INFO("INA226 Config: %04X\n", val);
			break;
			
				
		default:
			break;
	}		
}


/**
 * System initialization
 * Called once from user_init
 */

LOCAL void ICACHE_FLASH_ATTR sysInit(void)
{

	char *buf = util_zalloc(256); // Working buffer
	
	
	// I/O system initialization
	gpio_init();
	
	// Uart init
	uart0_init(BIT_RATE_115200);
	
	// I/O Pin initialization
	
	easygpio_pinMode(WIFI_LED, EASYGPIO_NOPULL, EASYGPIO_OUTPUT);
	GPIO_OUTPUT_SET(WIFI_LED, TRUE); 
	
	// I2c init
	i2c_master_gpio_init();
	
	os_delay_us(2000000); // To allow gtkterm to come up
	

	// Read in the config sector from flash
	configHandle = kvstore_open(KVS_DEFAULT_LOC);
	

	const char *ssidKey = commandElements[CMD_SSID].command;
	const char *WIFIPassKey = commandElements[CMD_WIFIPASS].command;
	const char *btLocalKey = commandElements[CMD_BTLOCAL].command;

	// Check for default configuration overrides
	if(!kvstore_exists(configHandle, ssidKey)){ // if no ssid, assume the rest of the defaults need to be set as well
		kvstore_put(configHandle, btLocalKey, configInfoBlock.e[MQTTBTLOCAL].value);
		kvstore_put(configHandle, ssidKey, configInfoBlock.e[WIFISSID].value);
		kvstore_put(configHandle, WIFIPassKey, configInfoBlock.e[WIFIPASS].value);

		// Write the KVS back out to flash	
	
		kvstore_flush(configHandle);
	}
	
	// Get the configurations we need from the KVS and store them in the commandElement data area
	
	kvstore_get_integer(configHandle, btLocalKey, &commandElements[CMD_BTLOCAL].p.i); // Retrieve button local
	
	commandElements[CMD_SSID].p.sp = kvstore_get_string(configHandle, ssidKey); // Retrieve SSID
	
	commandElements[CMD_WIFIPASS].p.sp = kvstore_get_string(configHandle, WIFIPassKey); // Retrieve WIFI Pass

	
	// Initialize MQTT connection 
	
	uint8_t *host = configInfoBlock.e[MQTTHOST].value;
	uint32_t port = (uint32_t) atoi(configInfoBlock.e[MQTTPORT].value);
	
	MQTT_InitConnection(&mqttClient, host, port,
	(uint8_t) atoi(configInfoBlock.e[MQTTSECUR].value));

	MQTT_InitClient(&mqttClient, configInfoBlock.e[MQTTDEVID].value, 
	configInfoBlock.e[MQTTUSER].value, configInfoBlock.e[MQTTPASS].value,
	atoi(configInfoBlock.e[MQTTKPALIV].value), 1);

	MQTT_OnConnected(&mqttClient, mqttConnectedCb);
	MQTT_OnDisconnected(&mqttClient, mqttDisconnectedCb);
	MQTT_OnPublished(&mqttClient, mqttPublishedCb);
	MQTT_OnData(&mqttClient, mqttDataCb);
	
	// Last will and testament

	os_sprintf(buf, "{\"muster\":{\"connstate\":\"offline\",\"device\":\"%s\"}}", configInfoBlock.e[MQTTDEVPATH].value);
	MQTT_InitLWT(&mqttClient, infoTopic, buf, 0, 0);

	// Subtopics
	commandTopic = util_make_sub_topic(configInfoBlock.e[MQTTDEVPATH].value, "command");
	statusTopic = util_make_sub_topic(configInfoBlock.e[MQTTDEVPATH].value, "status");
	INFO("Command subtopic: %s\r\n", commandTopic);
	INFO("Status subtopic: %s\r\n", statusTopic);
	
	// Attempt WIFI connection
	
	char *wifipass = commandElements[CMD_WIFIPASS].p.sp;
	char *ssid = commandElements[CMD_SSID].p.sp;
	
	INFO("Attempting connection with: %s\r\n", ssid);
	
	// Attempt to connect to AP
	WIFI_Connect(ssid, wifipass, wifiConnectCb);
	
	// Timer to execute code every 100 mSec
	
	
	
	// Free working buffer
	util_free(buf);
	
	// Start I2C state machine
	
	i2c_handler(NULL);
	
	INFO("\r\nSystem started ...\r\n");

}

/**
 * Called from startup
 */
 
void user_init(void)
{
	sysInit();
}
