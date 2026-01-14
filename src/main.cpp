/*
Requerimiento DSyM:
Lectura de BME280 + RTC + MicroSD Card en ESP32 
@author: Nicolas Pradenas - Ingeniero De proyectos (Supported By Gemini)
*/

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <SD.h>
#include <RTClib.h>

#include "logo.h" 

// ----------------------------------------------------
// DEFINICIONES Y OBJETOS
// ----------------------------------------------------
TFT_eSPI tft = TFT_eSPI();
Adafruit_BME280 bme;
RTC_DS3231 rtc;

#define SD_CS   32
#define SD_MOSI 26
#define SD_MISO 25
#define SD_SCK  33

SPIClass spiSD(VSPI);
File dataFile;

// BOTONES
#define BTN_RESET 0 
#define BTN_OK    36 
#define BTN_UP    37 
#define BTN_DOWN  38 

// ESTADOS DEL SISTEMA
enum SystemState { 
  STATE_MONITOR, 
  STATE_MENU,       
  STATE_CFG_TEMP,   
  STATE_CFG_HUM     
};

SystemState currentState = STATE_MONITOR;

// VARIABLES CONFIGURABLES
float tempMax = 28.0; 
float humMax  = 70.0;  

// VARIABLES DE SISTEMA
bool logging = false;
unsigned long lastSave = 0;
int fileCounter = 1;
unsigned long lastInputTime = 0; 
int menuIndex = 0; 
char currentFilename[32]; 

// Botones
unsigned long btnOkPressStart = 0;
bool btnOkPressed = false;
bool longPressHandled = false;

// Alarma
unsigned long lastBlink = 0;
bool blinkState = false;

// ----------------------------------------------------
// FUNCIONES UI
// ----------------------------------------------------

void dibujarHUD() {
  tft.fillRect(0, 0, 240, 20, TFT_BLACK); 
  tft.setTextSize(1);
  tft.setTextColor(TFT_SILVER, TFT_BLACK);

  DateTime now = rtc.now();
  tft.setCursor(5, 5);
  tft.printf("%02d:%02d", now.hour(), now.minute());

  if (logging) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(60, 5);
    tft.print("REC");
    tft.fillCircle(85, 8, 3, TFT_RED);
  } else {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(60, 5);
    tft.print("IDLE");
  }

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(110, 5);
  tft.printf("Mx:%.0f/%.0f", tempMax, humMax);
  
  tft.drawLine(0, 20, 240, 20, TFT_DARKGREY);
}

void mostrarMensaje(String msg, uint32_t color) {
    tft.fillScreen(TFT_BLACK);
    tft.drawRoundRect(20, 45, 200, 50, 5, color); 
    tft.setTextColor(color, TFT_BLACK); 
    tft.setTextSize(2);
    
    int xPos = 120 - (msg.length() * 6);
    tft.setCursor(xPos > 25 ? xPos : 25, 62);
    tft.print(msg);
    
    delay(1000);
    tft.fillScreen(TFT_BLACK);
}

void irADormir() {
    tft.fillScreen(TFT_BLACK);
    for(int i=0; i<135/2; i+=4) {
        tft.drawRect(0, i, 240, 135-(i*2), TFT_BLACK);
        delay(10);
    }
    tft.fillScreen(TFT_BLACK);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0); 
    esp_deep_sleep_start();
}

// ----------------------------------------------------
// SETUP
// ----------------------------------------------------
void setup() {
  Serial.begin(115200);
  tft.init(); 
  tft.setRotation(3); 
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true); 

  // --- 1. PANTALLA DE CARGA ---
  tft.fillScreen(TFT_BLACK);
  
  // A) LOGO
  tft.pushImage((240 - logoWidth) / 2, 10, logoWidth, logoHeight, logo_cctval);

  // B) TEXTO CCTVal (Azul)
  tft.setTextColor(TFT_BLUE, TFT_BLACK); 
  tft.setTextSize(2);
  int yText = 10 + logoHeight + 5;
  tft.setCursor(85, yText); 
  tft.print("CCTVal");

  // --- HARDWARE INIT ---
  Wire.begin(21, 22);
  bool err = false;
  if (!bme.begin(0x76)) err = true;
  if (!rtc.begin()) err = true;

  // Codigo para configurar hora y fecha actual
  // Cargar 1 vez y luego comentarla para que quede guardada en el RTC.
  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  
  spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  SD.begin(SD_CS, spiSD); 

  pinMode(BTN_RESET, INPUT_PULLUP); 
  pinMode(BTN_OK, INPUT_PULLUP);   
  pinMode(BTN_UP, INPUT_PULLUP); 
  pinMode(BTN_DOWN, INPUT_PULLUP);
  
  if(err) {
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.setCursor(20, yText + 30); tft.print("ERROR HW SENSOR");
      delay(3000);
  } else {
      delay(2000); 
  }
  
  tft.fillScreen(TFT_BLACK); 
  dibujarHUD();
}

// ----------------------------------------------------
// LOOP
// ----------------------------------------------------
void loop() {
  
  // =================================================
  // 1. MODO MONITOR
  // =================================================
  if (currentState == STATE_MONITOR) {
    
    // --- BOTÓN OK ---
    bool isPressed = (digitalRead(BTN_OK) == LOW);
    if (isPressed && !btnOkPressed) {
        btnOkPressed = true; btnOkPressStart = millis(); longPressHandled = false;
    } 
    else if (isPressed && btnOkPressed) {
        // ENTRAR AL MENU
        if (!longPressHandled && (millis() - btnOkPressStart > 3000)) {
            currentState = STATE_MENU; 
            menuIndex = 0; lastInputTime = millis(); longPressHandled = true; 
            
            // TRANSICIÓN: SOLO LOGO
            tft.fillScreen(TFT_BLACK); 
            tft.pushImage((240 - logoWidth) / 2, (135 - logoHeight)/2, logoWidth, logoHeight, logo_cctval);
            delay(1000); 
            tft.fillScreen(TFT_BLACK); 
            
            while(digitalRead(BTN_OK) == LOW) delay(10); 
            return; 
        }
    }
    else if (!isPressed && btnOkPressed) {
        if (!longPressHandled) {
             // START/STOP LOGGING
             logging = !logging;
             
             if (logging) {
                 if (!SD.exists("/") && !SD.begin(SD_CS, spiSD)) {
                     logging = false; mostrarMensaje("NO HAY SD!", TFT_RED);
                 } else {
                     while(true) {
                         sprintf(currentFilename, "/data_%03d.csv", fileCounter);
                         if(!SD.exists(currentFilename)) break;
                         fileCounter++;
                     }
                     dataFile = SD.open(currentFilename, FILE_WRITE);
                     if (dataFile) { 
                        if (dataFile.size() == 0) {
                            dataFile.println("Fecha,Hora,Temp_C,Hum_%,Pres_hPa,Nota"); 
                            dataFile.flush(); 
                        }
                        dataFile.close(); 
                        mostrarMensaje("GRABANDO", TFT_GREEN);
                     } else {
                        logging = false; mostrarMensaje("ERROR FILE", TFT_RED);
                     }
                 }
             } else {
                 if (dataFile) dataFile.close(); mostrarMensaje("PAUSA", TFT_RED);
             }
             dibujarHUD();
        }
        btnOkPressed = false;
    }

    // --- LECTURAS ---
    float temp = bme.readTemperature();
    float hum  = bme.readHumidity();
    float pres = bme.readPressure() / 100.0F;

    // --- ALARMAS ---
    bool alertTemp = (temp > tempMax);
    bool alertHum  = (hum > humMax);
    bool anyAlert  = alertTemp || alertHum;

    // --- DIBUJAR PANTALLA ---
    tft.fillRect(0, 30, 240, 180, TFT_BLACK); 

    if (anyAlert) {
        if (millis() - lastBlink > 800) { lastBlink = millis(); blinkState = !blinkState; }
        if (blinkState) {
            tft.drawRoundRect(20, 35, 200, 90, 5, TFT_RED);
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.setTextSize(2); tft.setCursor(80, 50); tft.print("ALERTA!");

            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            if (alertTemp && alertHum) { tft.setCursor(45, 80); tft.print("TEMP Y HUMEDAD"); }
            else if (alertTemp)        { tft.setCursor(50, 80); tft.print("TEMPERATURA"); }
            else if (alertHum)         { tft.setCursor(70, 80); tft.print("HUMEDAD"); }
            
            tft.setTextSize(2); 

            if (alertTemp && alertHum) {
                // CASO: AMBAS ALERTAS (Separadas por slash)
                tft.setCursor(35, 105);
                tft.printf("%.1fC / %.1f%%", tempMax, humMax);
            } 
            else {
                // CASO: SOLO UNA ALERTA
                tft.setCursor(75, 105);
                if (alertTemp) tft.printf("%.1f C", tempMax);
                if (alertHum)  tft.printf("%.1f %%", humMax);
            }
        } else { goto drawValues; }
    } else {
        drawValues:
        tft.setTextSize(2);
        tft.setCursor(20, 45); tft.setTextColor(TFT_GREEN, TFT_BLACK); tft.printf("T: %.1f C", temp);
        tft.setCursor(20, 80); tft.setTextColor(TFT_YELLOW, TFT_BLACK); tft.printf("H: %.1f %%", hum);
        tft.setCursor(20, 115); tft.setTextColor(TFT_MAGENTA, TFT_BLACK); tft.printf("P: %.0f hPa", pres);
    }
    dibujarHUD(); 

    // --- GUARDADO SD ---
    if (logging && (millis() - lastSave > 1000)) { 
       lastSave = millis();
       String nota = "";
       if (alertTemp && alertHum) nota = "ALERTA T+H";
       else if (alertTemp) nota = "ALERTA TEMP";
       else if (alertHum) nota = "ALERTA HUM";
       
       DateTime now = rtc.now();

       dataFile = SD.open(currentFilename, FILE_WRITE);
       if (dataFile) {
           if (dataFile.size() > 0) dataFile.seek(dataFile.size());
           
           dataFile.printf("%04d/%02d/%02d,%02d:%02d:%02d,%.2f,%.2f,%.1f,%s\n", 
                           now.year(), now.month(), now.day(),
                           now.hour(), now.minute(), now.second(),
                           temp, hum, pres, nota.c_str()); 
           dataFile.close();
       } else {
           logging = false; mostrarMensaje("SD ERROR", TFT_RED);
           spiSD.end(); spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
           dibujarHUD();
       }
    }
    delay(100);
  }

  // =================================================
  // 2. MODO MENÚ
  // =================================================
  else if (currentState == STATE_MENU) {
      if (millis() - lastInputTime > 30000) { currentState = STATE_MONITOR; tft.fillScreen(TFT_BLACK); return; }

      // Título MENU
      tft.setTextSize(2); tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.setCursor(95, 25); tft.print("MENU");
      tft.drawLine(20, 45, 220, 45, TFT_DARKGREY); 

      int yStart = 50; 
      int gap = 30; 
      tft.setTextSize(2);

      // Opción 0: Temp
      if (menuIndex == 0) {
          tft.setTextColor(TFT_GREEN, TFT_BLACK); 
          tft.setCursor(20, yStart); tft.printf("> TEMP MAX  %.1f", tempMax);
      } else {
          tft.setTextColor(TFT_DARKGREY, TFT_BLACK); 
          tft.setCursor(20, yStart); tft.printf("  TEMP MAX  %.1f", tempMax);
      }

      // Opción 1: Hum
      if (menuIndex == 1) {
          tft.setTextColor(TFT_GREEN, TFT_BLACK);
          tft.setCursor(20, yStart+gap); tft.printf("> HUM MAX   %.1f", humMax);
      } else {
          tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
          tft.setCursor(20, yStart+gap); tft.printf("  HUM MAX   %.1f", humMax);
      }

      // Opción 2: Apagar
      if (menuIndex == 2) {
          tft.setTextColor(TFT_RED, TFT_BLACK); 
          tft.setCursor(20, yStart+(gap*2)); tft.print("> APAGAR");
      } else {
          tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
          tft.setCursor(20, yStart+(gap*2)); tft.print("  APAGAR");
      }

      // Navegación
      if (digitalRead(BTN_DOWN) == LOW) { menuIndex++; if(menuIndex > 2) menuIndex = 0; lastInputTime = millis(); delay(200); }
      if (digitalRead(BTN_UP) == LOW) { menuIndex--; if(menuIndex < 0) menuIndex = 2; lastInputTime = millis(); delay(200); }

      // Selección
      if (digitalRead(BTN_OK) == LOW) {
          delay(150);
          if (menuIndex == 0) currentState = STATE_CFG_TEMP;
          else if (menuIndex == 1) currentState = STATE_CFG_HUM;
          else if (menuIndex == 2) irADormir();
          lastInputTime = millis(); tft.fillScreen(TFT_BLACK); 
          while(digitalRead(BTN_OK) == LOW) delay(10);
      }
  }

  // =================================================
  // 3. CONFIG TEMPERATURA
  // =================================================
  else if (currentState == STATE_CFG_TEMP) {
      if (millis() - lastInputTime > 30000) { currentState = STATE_MONITOR; return; }

      tft.setCursor(55, 30); tft.setTextSize(2); tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.print("TEMP MAX");
      tft.setCursor(75, 60); tft.setTextSize(3); tft.setTextColor(TFT_CYAN, TFT_BLACK); tft.printf("%.1f", tempMax);
      
      tft.setTextSize(1); tft.setTextColor(TFT_SILVER, TFT_BLACK);
      tft.setCursor(80, 105); tft.print("UP/DOWN: +/- 0.5");
      tft.setCursor(95, 120); tft.print("OK: SALIR");

      if (digitalRead(BTN_UP) == LOW)   { tempMax += 0.5; lastInputTime = millis(); delay(150); }
      if (digitalRead(BTN_DOWN) == LOW) { tempMax -= 0.5; lastInputTime = millis(); delay(150); }
      if (digitalRead(BTN_OK) == LOW) { delay(100); mostrarMensaje("GUARDADO", TFT_GREEN); currentState = STATE_MONITOR; dibujarHUD(); }
  }

  // =================================================
  // 4. CONFIG HUMEDAD
  // =================================================
  else if (currentState == STATE_CFG_HUM) {
      if (millis() - lastInputTime > 30000) { currentState = STATE_MONITOR; return; }

      tft.setCursor(65, 30); tft.setTextSize(2); tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.print("HUM MAX");
      tft.setCursor(75, 60); tft.setTextSize(3); tft.setTextColor(TFT_YELLOW, TFT_BLACK); tft.printf("%.1f", humMax); 
      
      tft.setTextSize(1); tft.setTextColor(TFT_SILVER, TFT_BLACK);
      tft.setCursor(80, 105); tft.print("UP/DOWN: +/- 0.5");
      tft.setCursor(95, 120); tft.print("OK: SALIR");

      if (digitalRead(BTN_UP) == LOW)   { humMax += 0.5; lastInputTime = millis(); delay(150); }
      if (digitalRead(BTN_DOWN) == LOW) { humMax -= 0.5; lastInputTime = millis(); delay(150); }
      if (digitalRead(BTN_OK) == LOW) { delay(100); mostrarMensaje("GUARDADO", TFT_GREEN); currentState = STATE_MONITOR; dibujarHUD(); }
  }
}

  // =================================================
  // FUNCIONES FUTURAS ....
  // =================================================