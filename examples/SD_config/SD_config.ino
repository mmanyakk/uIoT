#include <avr/wdt.h>
#include <petit_fatfs.h>
FATFS fs;          // Work area (file system object) for the volume
  
#define SD_SELECT A1
#define ETHERNET_SELECT 10
#define RFM12_SELECT 2
#define LED 5

#define UNO       //anti crash wachdog reset only works with Uno (optiboot) bootloader, comment out the line if using delianuova

//--------------------------------------------------------------------------
// Ethernet
//--------------------------------------------------------------------------
#include <EtherCard.h>		//https://github.com/jcw/ethercard 

//IP address of remote sever, only needed when posting to a server that has not got a dns domain name (staticIP e.g local server) 
byte Ethernet::buffer[700];
static uint32_t timer;
static BufferFiller bfill;  // used as cursor while filling the buffer

#include "RF12uiot.h"	  //Customized for uIoT pins


//---------------------------------------------------------------------
// The PacketBuffer class is used to generate the json string that is send via ethernet - JeeLabs
//---------------------------------------------------------------------
class PacketBuffer : public Print {
public:
    PacketBuffer () : fill (0) {}
    const char* buffer() { return buf; }
    byte length() { return fill; }
    void reset()
    { 
      memset(buf,NULL,sizeof(buf));
      fill = 0; 
    }
    virtual size_t write (uint8_t ch)
        { if (fill < sizeof buf) buf[fill++] = ch; }
    byte fill;
    char buf[150];
    private:
};
PacketBuffer str;

int ethernet_error = 0;                   // Etherent (controller/DHCP) error flag
int rf_error = 0;                         // RF error flag - high when no data received 
int ethernet_requests = 0;                // count ethernet requests without reply                 

int dhcp_status = 0;
int dns_status = 0;

int data_ready=0;                         // Used to signal that emontx data is ready to be sent
unsigned long last_rf;                    // Used to check for regular emontx data - otherwise error

char line_buf[50];                        // Used to store line of http reply header

unsigned long time60s = -50000;

byte needsave=0;          // EEPROM settings need saving?

#include <EEPROM.h>

// ID of the settings block
#define CONFIG_VERSION "mjh" //keep this 3 chars long
#define CONFIG_START 100    // Offset of the configuration in EEPROM

struct StoreStruct {
  // This is for mere detection if they are your settings
  char version[4];  //3+trailing zero
  byte band, group, nodeID, sendACK;
  byte ip[4], gw[4],  dns[4], hisip[4],mymac[6];
  byte usedhcp,usehisip;
  char website[24],api[33],basedir[24];
} storage = {
  CONFIG_VERSION,
  // The default values
  RF12_868MHZ, 210, 27, false,
  {192,168,1,35},{192,168,1,1},{8,8,8,8},{213,138,101,177},{ 0x42,0x31,0x42,0x21,0x30,0x31 },
  false,false,
  "emoncms.org","ff95de87c7ea7b114cdd99060a890274","/emoncms"
};

void loadConfig() {
  // To make sure there are settings, and they are ours
  // If nothing is found it will use the default settings.
  if (EEPROM.read(CONFIG_START + 0) == CONFIG_VERSION[0] &&
      EEPROM.read(CONFIG_START + 1) == CONFIG_VERSION[1] &&
      EEPROM.read(CONFIG_START + 2) == CONFIG_VERSION[2])
    for (unsigned int t=0; t<sizeof(storage); t++)
      *((char*)&storage + t) = EEPROM.read(CONFIG_START + t);
}

void saveConfig() {
  for (unsigned int t=0; t<sizeof(storage); t++)
    EEPROM.write(CONFIG_START + t, *((char*)&storage + t));
}


static int freeRam () {
  extern int __heap_start, *__brkval; 
  int v; 
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval); 
}



void setup()
{
  Serial.begin(9600);  
  
  pinMode(LED,OUTPUT);
  digitalWrite(LED,HIGH);
    
  delay(5000);
 
  if (EEPROM.read(CONFIG_START + 0) == CONFIG_VERSION[0] &&
      EEPROM.read(CONFIG_START + 1) == CONFIG_VERSION[1] &&
      EEPROM.read(CONFIG_START + 2) == CONFIG_VERSION[2])
     loadConfig();    //If these are our settings, load them
  else
     saveConfig();    //Else create settings with default values      
      
  strobe(SD_SELECT);  
  spi_init();

  Serial.println(F("[SD config]"));

  PFFS.begin(SD_SELECT, rx, tx);
  pf_mount(&fs);
  Serial.println(F("Reading SD config.."));   
  
  readConfig(PFFS.open_file("uiot.ini"), "uiot.ini");  //Try to fetch settings from SD card
    
  if(needsave==1) { // Only save to EEPROM if settings have changed
    Serial.println(F("Config changed. Saving new."));
    saveConfig();
  }

  Serial.println(F("RFM12B initialization"));      
  Serial.print(F("NodeID: ")); Serial.println(storage.nodeID);
  Serial.print(F("Band: ")); 

  static word bands[3] = { 433, 868, 915 };
  Serial.print(bands[storage.band-1],DEC);
  Serial.println(F(" MHz "));

  
  Serial.print(F("Group: ")); Serial.println(storage.group);

  rf12_initialize(storage.nodeID, storage.band, storage.group);  //RFM12b initialization MUST happen before ethernet..
  last_rf = millis()-40000;                                       // setting lastRF back 40s is useful as it forces the ethernet code to run straight away

  Serial.println(F("ENC28J60 initialization"));       
  if (ether.begin(sizeof Ethernet::buffer, storage.mymac, ETHERNET_SELECT) == 0) {	//for use with NanodeRF
    Serial.println(F("Failed to access Ethernet controller"));
    ethernet_error = 1;  
  }

  dhcp_status = 0;
  dns_status = 0;
  ethernet_requests = 0;
  ethernet_error=0;
  rf_error=0;
  
  if (storage.usedhcp == 0) 
    {
      memcpy(ether.dnsip,storage.dns,4);
      ether.staticSetup(storage.ip, storage.gw);    ether.printIp(F("IP:  "), ether.myip); ether.printIp(F("GW:  "), ether.gwip); ether.printIp(F("DNS: "), ether.dnsip);
      ether.copyIp(ether.hisip, storage.hisip); ether.printIp(F("Server: "), ether.hisip);
    }
    
    Serial.print(F("MAC: "));
    for (byte i = 0; i < 6; ++i) {
      Serial.print(storage.mymac[i], HEX);
    if (i < 5)
      Serial.print(':');
    }
    
  Serial.println();
 
  Serial.println(F("..done"));         

  Serial.print(F("API: "));
  Serial.println(storage.api);

  Serial.print(F("website: "));
  Serial.println(storage.website);

  Serial.print(F("basedir: "));
  Serial.println(storage.basedir);

  Serial.print(freeRam());
  Serial.println(F(" bytes of RAM free"));

//  ether.registerPingCallback(gotPinged);

  dhcp_status = 0;
  dns_status = 0;
  ethernet_requests = 0;
  ethernet_error=0;
  rf_error=0;
  
  digitalWrite(LED,LOW);                                    // Green LED off - indicate that setup has finished   
  #ifdef UNO
  wdt_enable(WDTO_8S); 
  #endif;
  
}
  
void loop () {
 
  #ifdef UNO
  wdt_reset();
  #endif

  dhcp_dns();   // handle dhcp and dns setup 

  
  // Display error states on status LED
  if (ethernet_error==1 || rf_error==1 || ethernet_requests > 0) digitalWrite(LED,HIGH);
      else digitalWrite(LED,LOW);

  //-----------------------------------------------------------------------------------------------------------------
  // 1) On RF recieve
  //-----------------------------------------------------------------------------------------------------------------
  checkRF();
  
  //-----------------------------------------------------------------------------------------------------------------
  // 2) If no data is recieved from rf12 module the server is updated every 30s with RFfail = 1 indicator for debugging
  //-----------------------------------------------------------------------------------------------------------------
  if ((millis()-last_rf)>30000 && data_ready==0)
  {
    last_rf = millis();                                                 // reset lastRF timer
    str.reset();                                                        // reset json string
    str.print(storage.basedir); str.print(F("/api/post.json?"));
    str.print(F("apikey=")); str.print(storage.api);
    str.print(F("&json={rf_fail:1}\0"));                                   // No RF received in 30 seconds so send failure 
    data_ready = 1;                                                     // Ok, data is ready
    rf_error=1;
  }

  //-----------------------------------------------------------------------------------------------------------------
  // 3) Send data via ethernet
  //-----------------------------------------------------------------------------------------------------------------

  word len = ether.packetReceive();
  word pos = ether.packetLoop(len);
  

  if (data_ready) {
    Serial.print(F("Data sent: ")); Serial.print(storage.website); Serial.println(str.buf); // print to serial json string

    // Example of posting to emoncms.org http://emoncms.org 
    // To point to your account just enter your WRITE APIKEY 
    ethernet_requests ++;    
    ether.browseUrl(PSTR("") ,str.buf, storage.website, my_callback);
    data_ready = 0;
  }

   
  if (ethernet_requests > 10) delay(10000); // Reset if more than 10 request attempts have been tried without a reply

  if ((millis()-time60s)>60000)
  {
    time60s = millis();                                                 // reset lastRF timer
    str.reset();
    str.print(storage.basedir); str.print(F("/time/local.json?"));str.print(F("apikey=")); str.print(storage.api);
    Serial.println(F("Time request sent"));
    ether.browseUrl(PSTR("") ,str.buf, storage.website, my_callback);
  }
}
//**********************************************************************************************************************

//-----------------------------------------------------------------------------------
// Ethernet callback
// recieve reply and decode
//-----------------------------------------------------------------------------------
static void my_callback (byte status, word off, word len) {
  int lsize =   get_reply_data(off);

  Serial.print(F("Got:"));     Serial.println(line_buf);
    
  if (strcmp(line_buf,"ok")==0)
  {
    Serial.println(F("OK recieved")); 
    ethernet_requests = 0; ethernet_error = 0;
  }
  else if(line_buf[0]=='t')
  {
    Serial.println(F("Time: "));
    Serial.println(line_buf);
    char tmp[] = {line_buf[1],line_buf[2],0};
    byte hour = atoi(tmp);
    tmp[0] = line_buf[4]; tmp[1] = line_buf[5];
    byte minute = atoi(tmp);
    tmp[0] = line_buf[7]; tmp[1] = line_buf[8];
    byte second = atoi(tmp);

    if (hour>0 || minute>0 || second>0) 
    {  
      char data[] = {'t',hour,minute,second};
      int i = 0; while (!rf12_canSend() && i<10) {rf12_recvDone(); i++;}
      rf12_sendStart(0, data, sizeof data);
      rf12_sendWait(0);
    }
  }
}

void checkRF() {
  
   if (rf12_recvDone() && rf12_crc == 0 && (rf12_hdr & RF12_HDR_CTL) == 0) {
      
        digitalWrite(LED,HIGH);
        int node_id = (rf12_hdr & 0x1F);
        byte n = rf12_len;
      
        if(RF12_WANTS_ACK){
           byte i = 0; while (!rf12_canSend() && i<10) {rf12_recvDone(); i++;}  // if ready to send 
           rf12_sendStart(RF12_ACK_REPLY, 0, 0);
           rf12_sendWait(2);           // Wait for RF to finish sending while in standby mode           
        }                 
        
        str.reset();
        
        str.print(storage.basedir); str.print(F("/api/post.json?"));
    
        str.print(F("apikey=")); str.print(storage.api);
        str.print(F("&node="));  str.print(node_id);
        str.print(F("&csv="));
        for (byte i=0; i<n; i+=2)
        {
          unsigned int num = ((unsigned char)rf12_data[i+1] << 8 | (unsigned char)rf12_data[i]);
          if (i) str.print(",");
          str.print(num);
        }

        str.print("\0");  //  End of json string
        data_ready = 1; 
        last_rf = millis(); 
        rf_error=0;
        
        delay(20);
        digitalWrite(LED, LOW);
      
  }
 
}


static void gotPinged(byte* ptr) {
//  ether.printIp(">>> ping from: ",ptr);
}


