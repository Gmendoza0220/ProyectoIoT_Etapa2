// Librerías a utilizar
#include "MatrixKeypad.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include <Servo.h> // Librería para el Servo

// Configuración de red WiFi ---------------------------------------------------------------------
const char* ssid = "<Your wifi name>"; // Cambia "TuSSID" por el nombre de tu red WiFi
const char* password = "<Your wifi password>"; // Cambia "TuContraseña" por la contraseña de tu red

// Configuración del servidor MQTT ---------------------------------------------------------------------
const char* mqttServer = "mqtt.eclipseprojects.io"; // Broker público MQTT
const int mqttPort = 1883;                     // Puerto del servidor MQTT
const char* idUserTopic = "<Your iduser topic>";
const char* wrongPassTopic = "<Your wrongPass topic>";
const char* entradaSalidaTopic = "<Your entryExit topic>";

// WiFi y MQTT ---------------------------------------------------------------------
WiFiClient espClient;
PubSubClient client(espClient);

// LCD ---------------------------------------------------------------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Keypad ---------------------------------------------------------------------------------
const byte rown = 4, coln = 4;
char keymap[rown][coln] = {
    {'1', '2', '3', '#'},
    {'4', '5', '6', '#'},
    {'7', '8', '9', '#'},
    {'#', '0', '#', '#'}};
byte rowPins[rown] = {2, 3, 4, 5};
byte colPins[coln] = {6, 7, 8, 9};
MatrixKeypad_t* keypad;

// SERVO ----------------------------------------------------------------------------------
Servo myservo;
int pos = 0;
bool door = false;

// EXTRA ----------------------------------------------------------------------------------
int buzzer = A0;
int ledRojo = A1;
int ledVerde = A2;

// Variables globales ---------------------------------------------------------------------
// EnteredID: Almacenará la ID del usuario durante el loop().
// enteredPassword: Almacenará la contraseña que se ingresó desde el pad númerico.
// visualPass: Representa la contraseña con "*" para ser mostrada en pantalla .
String enteredID = "", enteredPassword = "", visualPass = ""; 
// expectedPassword: Es la password del usuario encontrado gracias a su ID.
// userName: El nombre del usuario encontrado gracias a su ID.
String expectedPassword = "", userName = "";
int attempts = 0; // Número de intentos para ingresar la contraseña.
bool idValid = false; // Verifica que la id sea válida.
bool esperandoContrasena = false; // Sirve para saber sí es que se está esperando la password del usuario.
bool esperandoCallback = false; // Permitirá ejecutar solo el callback de fondo para recibir el json.
bool callbackActivo = false; // Permite ejecutar el callback solo cuando sea necesario.

unsigned long lastReconnectAttempt = 0; // Representa los intentos de conexión

// LOGICA WIFI ---------------------------------------------------------------------

// Función para conectar a WiFi -----------------------------------------------------------------
void connectWiFi() {
  // Se indica que se está conectando a WiFi
  lcd.clear();
  lcd.print("Conectando WiFi");
  WiFi.begin(ssid, password);
  unsigned long startAttemptTime = millis();

  // Imprime "." mientras la conexión aun no esté establecida
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 30000) {
    delay(1000);
    Serial.print(".");
    lcd.print(".");
  }

  // En caso que la conexión ya está establecida
  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear();
    lcd.print("WiFi conectado");
    Serial.println("\nConectado a WiFi");
  } else {
    // En caso contrario, se imprime el error.
    lcd.clear();
    lcd.print("Error WiFi");
    Serial.println("\nNo se pudo conectar a WiFi");
  }
}

// Función para reconectar MQTT -----------------------------------------------------------------
bool reconnectMQTT() {
  // Devuelve "true" si es que la conexión con MQTT es exitosa
  if (client.connected()) return true;

  // En caso contrario, se prepara la reconección
  static int retryCount = 0;
  retryCount++;
  Serial.print("Conectando a MQTT (intento ");
  Serial.print(retryCount); // Se imprime el número de intentos de conexión actuales
  Serial.println(")...");

  // Se verifica que se haya establecido la conexión con el Arduino

  // Cuando se trabajaba con test.mosquitto.org
  // if (client.connect(("ArduinoClient_" + String(random(0, 9999))).c_str())) { 

  if (client.connect("<Your unique client id>")) { 
    client.subscribe("<Your topic sendData>");
    Serial.println(" Conectado y suscrito.");
    retryCount = 0; // Reinicia el contador si conecta
    return true;
  } else {
    // En caso contrario, imprime el código de error del cliente SubPubClient.
    Serial.print(" Error MQTT: ");
    Serial.println(client.state());
    delay(5000); // Evita intentos rápidos consecutivos
    return false;
  }
}

// Mantenimiento de conexiones WiFi y MQTT -----------------------------------------------------------------
void maintainConnections() {
  // Verifica que se mantenga la conexión con WiFi
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  // Verifica que se mantenga la conexión con MQTT
  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      if (reconnectMQTT()) lastReconnectAttempt = 0;
    }
  }
}

// PERMITIRÁ ENVIAR LA ID DEL USUARIO PARA LA BUSQUEDA DE SUS DATOS -----------------------------------------------------------------
void enviarID(const String& id) {
  // Verifica que se esté conectado a MQTT
  if (!client.connected()) {
    Serial.println("No conectado a MQTT. Intentando reconectar...");
    if (!reconnectMQTT()) {
      // En caso contrario, se reinicia el sistema al momento que se ingresa la ID.
      Serial.println("Fallo en reconexión. ID no enviado.");
      resetToIDEntry();
      return;
    }
  }

  // Intentar enviar ID por el tópico correspondiente
  if (client.publish(idUserTopic, id.c_str())) {
    Serial.print("ID enviada correctamente: ");
    Serial.println(id);
    callbackActivo = true; // Se activa el callback
    esperandoCallback = true; // Se indica que se está esperando el callback
  } else {
    // En caso contrario, se llama a otro método para reintentar nuevamente el mensaje.
    Serial.println("Error al enviar ID. Reintentando...");
    reintentarPublicacion(id, idUserTopic);
  }
}

// PERMITIRÁ ENVIAR LA ID DEL USUARIO PARA REGISTRAR UNA ENTRADA o SALIDA -----------------------------------------------------------------
void enviarEntryExit(const String& iduser_EntryExit) {
  if (!client.connected()) {
    Serial.println("No conectado a MQTT. Intentando reconectar...");
    if (!reconnectMQTT()) {
      Serial.println("Fallo en reconexión. ID no enviado.");
      return;
    }
  }

  // Intentar enviar ID
  if (client.publish(entradaSalidaTopic, iduser_EntryExit.c_str())) {
    Serial.print("ID enviada correctamente: ");
    Serial.println(iduser_EntryExit);
  } else {
    Serial.println("Error al enviar ID. Reintentando...");
    reintentarPublicacion(iduser_EntryExit, entradaSalidaTopic);
  }
}

// PERMITIRÁ ENVIAR LOS DATOS DE INICIO SE SESIÓN ERRÓNEO -----------------------------------------------------------------
void enviarInvalidUserData(const String& invalidUserData) {
  if (!client.connected()) {
    Serial.println("No conectado a MQTT. Intentando reconectar...");
    if (!reconnectMQTT()) {
      Serial.println("Fallo en reconexión. ID no enviado.");
      return;
    }
  }

  // Intentar enviar los datos del intento inválido
  if (client.publish(wrongPassTopic, invalidUserData.c_str())) {
    Serial.print("ID enviada correctamente: ");
    Serial.println(invalidUserData);
  } else {
    Serial.println("Error al enviar ID. Reintentando...");
    reintentarPublicacion(invalidUserData, wrongPassTopic);
  }
}

// PERMITIRÁ REENVIAR UN MENSAJE POR EL TOPICO QUE SE LE ASIGNE -----------------------------------------------------------------
void reintentarPublicacion(const String& mensaje, const char* topico) {
  for (int i = 0; i < 3; i++) { // Máximo 3 intentos
    delay(1000); // Espera breve entre intentos
    if (client.publish(topico, mensaje.c_str())) {
      Serial.print("Reintento exitoso: ");
      Serial.println(mensaje);
      return;
    }
    Serial.print("Reintento fallido (intento ");
    Serial.print(i + 1);
    Serial.println(")");
  }
  // En caso de no lograr enviar el mensaje, se reinicia el sistema nuevamente.
  Serial.println("No se pudo enviar el mensaje después de varios intentos.");
  resetToIDEntry();
}

// LOGICA DEL PROGRAMA ------------------------------------------------------

/*--- Cerrar la puerta (servo) ---*/
void ServoClose() {
  for (pos = 90; pos >= 30; pos -= 10) {
    myservo.write(pos);
    delay(100);
  }
}

/*--- Abrir la puerta (servo) ---*/
void ServoOpen() {
  for (pos = 30; pos <= 120; pos += 10) {
    myservo.write(pos);
    delay(100);
  }
}

/*--- Repetirá una alarma para el buzzer y LED rojo ---*/
void alarma() {
  for (int i = 0; i < 5; i++) {  // Repetir la alarma 5 veces
    digitalWrite(ledRojo, HIGH);  // Encender el LED rojo
    tone(buzzer, 1000);  // Emitir sonido en el buzzer a 1000 Hz
    delay(500);  // Esperar 500ms
    digitalWrite(ledRojo, LOW);  // Apagar el LED rojo
    noTone(buzzer);  // Apagar el sonido del buzzer
    delay(500);  // Esperar otros 500ms
  }
}

/*--- Muestra un mensaje de carga en el monitor y LCD ---*/
void loading(char msg[]) {
  Serial.print(msg);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(msg);
  for (int i = 0; i < 6; i++) {
    delay(1000);
    Serial.print(".");
    lcd.print(".");
  }
  Serial.println("");
}

// SETUP -----------------------------------------------------------------
void setup() { 
  Serial.begin(9600);

  // Configuración de LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Iniciando...");

  // Configuración de Keypad
  keypad = MatrixKeypad_create((char*)keymap, rowPins, colPins, rown, coln);

  myservo.attach(13); // El servomotor se vincula al pin 13.

  //Se definen los modos de pin a los pines utilizados para los LEDs y el Buzzer.
  pinMode(buzzer, OUTPUT);
  pinMode(ledRojo, OUTPUT);
  pinMode(ledVerde, OUTPUT);

  ServoClose(); // Cerramos el servo al iniciar el sistema

  // Conexión WiFi
  connectWiFi();

  // Configuración de cliente MQTT
  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);

  // Conexión inicial MQTT
  reconnectMQTT();

  lcd.clear();
  lcd.print("Ingrese su ID:");
}

// LOOP  -----------------------------------------------------------------
void loop() {
  // Verificar conexión WiFi y MQTT
  maintainConnections();
  client.loop();

  // Se verifica que el callback esté desactivado antes de ingresar
  // id y contraseña.
 if (!esperandoCallback && !callbackActivo) {
    if (!esperandoContrasena) {
      // En caso de que no se espere la contraseña, se recibe la id.
      char customKey = MatrixKeypad_waitForKey(keypad);
      ingresarID(customKey);
    } else if (idValid) {
      // En caso de que la id sea válida y se haya recibido la info del usuario,
      // se entrará en modo contraseña.
      char customKey = MatrixKeypad_waitForKey(keypad);
      ingresarContrasena(customKey);
    }
  }
}

// MÉTODO CALLBACK PARA RECIBIR DATOS DE UN TÓPICO -----------------------------------------------------------------
void callback(char* topic, byte* payload, unsigned int length) {
  esperandoCallback = false; // Ya se recibió el mensaje

  // En caso que el callback no esté activo
  if (!callbackActivo) {
    Serial.println("Callback desactivado. Mensaje ignorado.");
    return;
  }

  // Se extrae el mensaje recibido por el payload.
  char message[length + 1];
  strncpy(message, (char*)payload, length);
  message[length] = '\0';

  // Se muestra el mensaje recibido
  Serial.print("Mensaje recibido por el topico: ");
  Serial.println(topic);
  Serial.print("Mensaje: ");
  Serial.println(message);

  // Si el mensaje es "not execute", significa que el tópico no a enviado los datos del usuario.
  if (strcmp(message, "not execute") == 0) {
    Serial.println("Mensaje 'not execute' recibido. No se ejecuta nada.");
    resetToIDEntry(); // Evita reinicios múltiples
    callbackActivo = false; // Desactiva el callback hasta que sea necesario
    return;
  }

  // Procesar JSON si no es "not execute"
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    Serial.println("Error al procesar JSON");
    resetToIDEntry(); // Solo reinicia si hay error
    return;
  }

  // Asignar valores del JSON
  const char* nombre_usuario = doc["nombre_usuario"];
  const char* contrasena = doc["contrasena"];

  // En caso de que se tenga el usuario y contraseña...
  if (nombre_usuario && contrasena) {
    // Se asigna la password esperada junto a su usuario correspondiente.
    userName = nombre_usuario;
    expectedPassword = contrasena;
    idValid = true; // la id fue validada
    esperandoContrasena = true; // se está esperando la contraseña
    callbackActivo = false; // el callback ya no está activo
    // Se muestra un mensaje en pantalla con el nombre del usuario encontrado.
    lcd.clear(); 
    lcd.print("Bienvenido/a ");
    lcd.setCursor(0, 1);
    lcd.print(userName);
    Serial.println("ID válida, esperando contraseña...");
    delay(2000);
    lcd.clear();
    lcd.print("Ingrese su pass:");
    // Aquí se continua en el loop...
  } else {
    // En caso de que el nombre_usuario sea "Usuario no encontrado",
    // se reinicia el sistema para que se ingrese nuevamente una id.
    lcd.clear();
    callbackActivo = false;
    lcd.print("Usuario invalido");
    Serial.println("Usuario no encontrado.");
    delay(2000);
    resetToIDEntry();
  }
}

// Método que recibe la ID por el teclado númerico -----------------------------------------------------------------
void ingresarID(char key) { 
  // Se verifica que no se esté en modo contraseña.
  if (esperandoContrasena) {
    Serial.println("Modo contraseña activo, no se permite ingresar ID.");
    return; // Salir de la función si se espera contraseñaº
  }

  if (key == '#') { // Enviar ID cuando se presiona '#'
    if (!enteredID.isEmpty()) {
      lcd.clear();
      lcd.print("Enviando id");
      enviarID(enteredID);
    } else {
      Serial.println("ID vacío. No se envía.");
    }
  } else if (enteredID.length() < 16) { // Construir ID
    enteredID += key;
    lcd.setCursor(0, 1);
    lcd.print(enteredID);
    Serial.print("ID ingresada: ");
    Serial.println(enteredID);
  } else { // Reiniciar el sistema en caso que la id sobrepase los 16 carácteres.
    lcd.clear();
    lcd.print("Máx. 16");
    Serial.println("ID excede límite de caracteres.");
    delay(500);
    resetToIDEntry();
  }
}

// Función para ingresar contraseña -----------------------------------------------------------------
void ingresarContrasena(char key) {
  if (key == '#') { // Enviar la contraseña al presionar "#"
    // Se verifica que la id del usuario actual sea la misma que se ingresó desde el pad númerico.
    if (enteredPassword == expectedPassword) {
      // En caso de que si, se indica que se permite el acceso.
      lcd.clear();
      lcd.print("Acceso permitido");
      enviarEntryExit(enteredID); // Se envía la id del usuario para marcar una ENTRADA o SALIDA.
      delay(1000);
      ServoOpen(); // Abrir la puerta
      digitalWrite(ledVerde, HIGH); // Pender la luz de pase
      tone(buzzer, 500, 1000); // Sonido para indicar el paso
      loading("Esperando");
      lcd.clear();
      lcd.print("Cerrando puerta");
      tone(buzzer, 500, 1000); // Sonido para indicar el cierre
      ServoClose();
      digitalWrite(ledVerde, LOW); // Se apaga la luz de pase
      resetToIDEntry(); // Se reinicia el sistema
    } else {
      attempts++; // Se suma un intento
      // Se verifica que sigan quedando 3 intentos.
      if (attempts >= 3) { 
        // En caso de que se hayan acabado, se envía un archivo json
        // con la información del intento erróneo.
        lcd.clear();
        lcd.print("Sin intentos");
        alarma(); // Suena una alarma indicando que se agotaron los intentos
        StaticJsonDocument<100> errorDoc;
        errorDoc["iduser"] = enteredID;
        errorDoc["wrongPass"] = enteredPassword;
        String jsonString;
        serializeJson(errorDoc, jsonString);
        enviarInvalidUserData(jsonString); // Se pública el mensaje
        delay(1000);
        resetToIDEntry(); // Se reinicia el sistema
      } else {
        // En caso de que se haya equivocado de contraseña, se carga un nuevo intento.
        lcd.clear();
        lcd.print("Pass incorrecta");
        tone(buzzer, 500, 1000); // Sonido para indicar el fallo
        digitalWrite(ledRojo, HIGH); // Luz para indicar el fallo
        delay(2000);
        digitalWrite(ledRojo, LOW); // Se apaga la luz al cargar el nuevo intento
        lcd.clear();
        lcd.print("Ingrese otra vez");
      }
    }
    // Se reinicia la contraseña ingresada
    enteredPassword = "";
    visualPass = "";
  } else {
    // Se van agregando los carácteres de la contraseña.
    enteredPassword += key;
    visualPass += "*";
    lcd.setCursor(0, 1);
    lcd.print(visualPass);
  }
}

// Reiniciar a ID -----------------------------------------------------------------
void resetToIDEntry() {
  // Indica un mensaje de reinicio
  lcd.clear();
  lcd.print("Reiniciando");
  Serial.println("Reiniciando");
  delay(500);

  // Se reinician todas las variables
  enteredID = "";
  enteredPassword = "";
  visualPass = "";
  expectedPassword = "";
  userName = "";
  attempts = 0;
  idValid = false;
  esperandoContrasena = false; // Volver a modo ingreso de ID

  // Se regresa al modo de ingreso de id.
  lcd.clear();
  lcd.print("Ingrese su ID:");
  Serial.println("Ingrese su ID:");
}