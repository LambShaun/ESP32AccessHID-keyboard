#include <Arduino.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <Preferences.h>
#include "USBHID.h"

#define JOYSTICK_X_PIN 2
#define JOYSTICK_Y_PIN 1
#define JOYSTICK_SW_PIN 42
#define PROFILE_BUTTON_PIN 41
#define BUTTON1_PIN 6
#define BUTTON2_PIN 7
#define BUTTON3_PIN 15
#define BUTTON4_PIN 16
#define BUTTON5_PIN 17
#define BUTTON6_PIN 18
#define BUTTON7_PIN 40
#define BUTTON8_PIN 39
#define BUTTON9_PIN 38

#define LED_PIN LED_BUILTIN  // 使用板载 LED

// 常量定义
const int CUSTOM_BUTTON_PINS[] = { BUTTON1_PIN, BUTTON2_PIN, BUTTON3_PIN, BUTTON4_PIN, BUTTON5_PIN, BUTTON6_PIN, BUTTON7_PIN, BUTTON8_PIN, BUTTON9_PIN };
const int NUM_CUSTOM_BUTTONS = sizeof(CUSTOM_BUTTON_PINS) / sizeof(CUSTOM_BUTTON_PINS[0]);
const int NUM_PROFILES = 5;
const int ADC_MAX = 4095;
const int ADC_CENTER = ADC_MAX / 2;
const int ADC_DEADZONE = 500;
const int ADC_THRESHOLD_LOW = ADC_CENTER - ADC_DEADZONE;
const int ADC_THRESHOLD_HIGH = ADC_CENTER + ADC_DEADZONE;

// 全局对象和变量
USBHIDKeyboard Keyboard;
Preferences preferences;
int currentProfile = 0;
const char *PREFERENCES_NS = "gamepadConfig";
const char *PROFILE_INDEX_KEY = "currentProfile";
bool inConfigMode = false;

struct KeyMapping {
  uint8_t joystickButton;
  uint8_t customButtons[NUM_CUSTOM_BUTTONS];
};
KeyMapping profiles[NUM_PROFILES];

struct ButtonState {
  int pin;
  bool lastState;
  bool currentState;
  unsigned long lastDebounceTime;
  uint8_t mappedKey;
  bool isPressed;
};
ButtonState joystickButtonState;
ButtonState customButtonStates[NUM_CUSTOM_BUTTONS];
ButtonState profileButtonState;
unsigned long debounceDelay = 50;

bool w_pressed = false;
bool a_pressed = false;
bool s_pressed = false;
bool d_pressed = false;

// 非阻塞 LED 闪烁状态变量
bool isBlinkingProfile = false;         // Profile 指示灯是否正在闪烁
int blinksToDo = 0;                     // 剩余闪烁次数
unsigned long lastBlinkChangeTime = 0;  // 上次 LED 状态改变时间
bool currentLedState = LOW;             // LED 当前状态
int blinkOnDuration = 300;              // 非阻塞闪烁亮灯时长
int blinkOffDuration = 250;             // 非阻塞闪烁灭灯时长

// 函数声明
void loadProfile(int);
void saveProfileIndex(int);
void saveAllProfilesToNVS();
void setupDefaultProfiles();
void readAndUpdateButton(ButtonState &);
void handleJoystickMovement(int, int);
void handleButtonPress(ButtonState &);
void pressMappedKey(uint8_t key);
void releaseMappedKey(uint8_t key);
void blinkLedBlocking(int times, int onDuration = 100, int offDuration = 100);  // 用于 setup 中的简单阻塞闪烁
void startProfileBlink(int times);                                              // 开始非阻塞闪烁
void handleProfileBlink();                                                      // 处理非阻塞闪烁逻辑
void runNormalMode();
void runConfigMode();
void showHelp();
void showProfile(int);
bool setKeyMap(int, String, int, String);
uint8_t parseKeyCode(String);


//Setup
void setup() {
  Serial.begin(115200);
  pinMode(PROFILE_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (digitalRead(PROFILE_BUTTON_PIN) == LOW) {
    inConfigMode = true;
    Serial.println("\n\n PROFILE 键被按下，进入配置模式... ");
    // 使用简单的阻塞闪烁指示进入配置模式
    for (int i = 0; i < 3; ++i) {
      blinkLedBlocking(2, 50, 50);  // led闪烁两次
      delay(150);
    }
  } else {
    inConfigMode = false;
    Serial.println("\n 无障碍游戏控制器启动 (HID 模式) ");
  }

  pinMode(JOYSTICK_SW_PIN, INPUT_PULLUP);
  for (int i = 0; i < NUM_CUSTOM_BUTTONS; i++) pinMode(CUSTOM_BUTTON_PINS[i], INPUT_PULLUP);

  preferences.begin(PREFERENCES_NS, false);
  currentProfile = preferences.getInt(PROFILE_INDEX_KEY, 0);
  if (currentProfile < 0 || currentProfile >= NUM_PROFILES) currentProfile = 0;
  Serial.printf(" 当前 Profile 索引: %d\n", currentProfile);
  setupDefaultProfiles();
  loadProfile(currentProfile);

  joystickButtonState = { JOYSTICK_SW_PIN, digitalRead(JOYSTICK_SW_PIN), digitalRead(JOYSTICK_SW_PIN), 0, profiles[currentProfile].joystickButton, false };
  profileButtonState = { PROFILE_BUTTON_PIN, digitalRead(PROFILE_BUTTON_PIN), digitalRead(PROFILE_BUTTON_PIN), 0, 0, false };
  for (int i = 0; i < NUM_CUSTOM_BUTTONS; i++) {
    customButtonStates[i] = { CUSTOM_BUTTON_PINS[i], digitalRead(CUSTOM_BUTTON_PINS[i]), digitalRead(CUSTOM_BUTTON_PINS[i]), 0, profiles[currentProfile].customButtons[i], false };
  }

  if (!inConfigMode) {
    Keyboard.begin();
    USB.begin();
    Serial.println(" USB HID 设备已启动 ");
    delay(500);  // 等待USB稳定
    // 启动时，开始非阻塞闪烁指示当前 Profile
    startProfileBlink(currentProfile + 1);  // 调用新的启动函数
  } else {
    Serial.println(" 配置模式下 USB HID 未启动 ");
    showHelp();
  }
}

void loop() {
  if (inConfigMode) {
    runConfigMode();
  } else {
    runNormalMode();
  }
}

// 正常 HID 模式逻辑 (使用非阻塞闪烁)
void runNormalMode() {
  handleProfileBlink();  // 在每次循环中处理当前的闪烁状态

  // 读取输入
  int joyX = analogRead(JOYSTICK_X_PIN);
  int joyY = analogRead(JOYSTICK_Y_PIN);
  readAndUpdateButton(joystickButtonState);
  readAndUpdateButton(profileButtonState);
  for (int i = 0; i < NUM_CUSTOM_BUTTONS; i++) {
    readAndUpdateButton(customButtonStates[i]);
  }

  // 检查 Profile 切换按钮
  static bool profileButtonPressed = false;
  if (profileButtonState.currentState == LOW && !profileButtonPressed) {
    profileButtonPressed = true;
  } else if (profileButtonState.currentState == HIGH && profileButtonPressed) {
    profileButtonPressed = false;  // 重置按钮按下状态

    // 切换 Profile
    // 1. 停止任何正在进行的 Profile 闪烁
    if (isBlinkingProfile) {
      isBlinkingProfile = false;
      digitalWrite(LED_PIN, LOW);
      currentLedState = LOW;
      blinksToDo = 0;
      Serial.println("Profile switch interrupted ongoing blink.");
    }

    // 2. 更新 Profile
    currentProfile = (currentProfile + 1) % NUM_PROFILES;
    saveProfileIndex(currentProfile);
    loadProfile(currentProfile);
    Serial.printf(" 切换到 Profile: %d\n", currentProfile + 1);

    // 3. 重置按键状态
    Keyboard.releaseAll();
    joystickButtonState.isPressed = false;
    w_pressed = a_pressed = s_pressed = d_pressed = false;

    // 4. 开始新的非阻塞闪烁
    startProfileBlink(currentProfile + 1);
  }

  // 处理摇杆和按键(可以在闪烁时进行)
  handleJoystickMovement(joyX, joyY);
  handleButtonPress(joystickButtonState);
  for (int i = 0; i < NUM_CUSTOM_BUTTONS; i++) {
    handleButtonPress(customButtonStates[i]);
  }

  delay(5);
}

// 配置模式逻辑
void runConfigMode() {
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    Serial.print("收到命令: ");
    Serial.println(command);

    String commandUpper = command;
    commandUpper.toUpperCase();

    if (commandUpper == "HELP") {
      showHelp();
    } else if (commandUpper.startsWith("SHOW ")) {
      int profileToShow = command.substring(5).toInt();
      if (profileToShow >= 1 && profileToShow <= NUM_PROFILES) {
        showProfile(profileToShow - 1);
      } else {
        Serial.println("无效的 Profile 编号 (1-5).");
      }
    } else if (commandUpper.startsWith("P")) {
      int profileNum = command.substring(1, 2).toInt();
      if (profileNum >= 1 && profileNum <= NUM_PROFILES) {
        int space1 = command.indexOf(' ', 2);
        int space2 = -1;
        if (space1 > 1) space2 = command.indexOf(' ', space1 + 1);

        if (space1 > 1 && space2 > space1) {
          String targetStr = command.substring(space1 + 1, space2);
          String keyStr = command.substring(space2 + 1);
          targetStr.trim();
          keyStr.trim();

          if (keyStr.length() == 0) {
            Serial.println("错误: 缺少按键名称或代码.");
          } else {
            String targetType = "";
            int targetIndex = 0;
            String targetStrUpper = targetStr;
            targetStrUpper.toUpperCase();

            if (targetStrUpper == "JOY") {
              targetType = "joy";
            } else if (targetStrUpper.startsWith("B")) {
              targetIndex = targetStr.substring(1).toInt();
              if (targetIndex >= 1 && targetIndex <= NUM_CUSTOM_BUTTONS) {
                targetType = "btn";
              } else {
                Serial.print("错误: 无效的按钮索引 (B1 - B");
                Serial.print(NUM_CUSTOM_BUTTONS);
                Serial.println(").");
              }
            } else {
              Serial.println("错误: 无效的目标 ('JOY' 或 'B1'-'B" + String(NUM_CUSTOM_BUTTONS) + "').");
            }

            if (targetType != "") {
              if (setKeyMap(profileNum - 1, targetType, targetIndex, keyStr)) {
                Serial.println("设置成功 (尚未保存).");
              }
            }
          }
        } else {
          Serial.println("无效的设置命令格式. 需要格式: P<num> <target> <key>");
          Serial.println("例如: P1 B1 a  或  P2 JOY LSHIFT");
        }
      } else {
        Serial.println("无效的 Profile 编号 (P1 - P5).");
      }
    } else if (commandUpper == "SAVE") {
      Serial.println("正在保存所有 Profiles 的当前映射到 NVS...");
      saveAllProfilesToNVS();
      Serial.println("保存完成.");
    } else if (commandUpper == "RESET") {
      Serial.println("将所有 Profiles 的映射重置为默认值 (尚未保存)...");
      setupDefaultProfiles();
      Serial.println("重置完成. 如需持久化请使用 'save'.");
    } else if (commandUpper == "EXIT") {
      Serial.println("收到 EXIT 命令，正在重启设备...");
      delay(100);
      ESP.restart();
    } else {
      Serial.println("未知命令. 输入 'help' 获取帮助.");
    }
  }
  // 在配置模式下慢闪 LED
  digitalWrite(LED_PIN, HIGH);
  delay(100);
  digitalWrite(LED_PIN, LOW);
  delay(900);
}


// 使用 switch 语句根据按键码调用对应的 Keyboard.press(常量)
void pressMappedKey(uint8_t key) {
  // 如果 key 是 0 （未映射），则不执行任何操作
  if (key == 0) return;

  Serial.printf("--> Workaround Press: 0x%02X\n", key);
  switch (key) {
    case 'a':
    case 'A': Keyboard.press('a'); break;
    case 'b':
    case 'B': Keyboard.press('b'); break;
    case 'c':
    case 'C': Keyboard.press('c'); break;
    case 'd':
    case 'D': Keyboard.press('d'); break;
    case 'e':
    case 'E': Keyboard.press('e'); break;
    case 'f':
    case 'F': Keyboard.press('f'); break;
    case 'g':
    case 'G': Keyboard.press('g'); break;
    case 'h':
    case 'H': Keyboard.press('h'); break;
    case 'i':
    case 'I': Keyboard.press('i'); break;
    case 'j':
    case 'J': Keyboard.press('j'); break;
    case 'k':
    case 'K': Keyboard.press('k'); break;
    case 'l':
    case 'L': Keyboard.press('l'); break;
    case 'm':
    case 'M': Keyboard.press('m'); break;
    case 'n':
    case 'N': Keyboard.press('n'); break;
    case 'o':
    case 'O': Keyboard.press('o'); break;
    case 'p':
    case 'P': Keyboard.press('p'); break;
    case 'q':
    case 'Q': Keyboard.press('q'); break;
    case 'r':
    case 'R': Keyboard.press('r'); break;
    case 's':
    case 'S': Keyboard.press('s'); break;
    case 't':
    case 'T': Keyboard.press('t'); break;
    case 'u':
    case 'U': Keyboard.press('u'); break;
    case 'v':
    case 'V': Keyboard.press('v'); break;
    case 'w':
    case 'W': Keyboard.press('w'); break;
    case 'x':
    case 'X': Keyboard.press('x'); break;
    case 'y':
    case 'Y': Keyboard.press('y'); break;
    case 'z':
    case 'Z': Keyboard.press('z'); break;
    case '0': Keyboard.press('0'); break;
    case '1': Keyboard.press('1'); break;
    case '2': Keyboard.press('2'); break;
    case '3': Keyboard.press('3'); break;
    case '4': Keyboard.press('4'); break;
    case '5': Keyboard.press('5'); break;
    case '6': Keyboard.press('6'); break;
    case '7': Keyboard.press('7'); break;
    case '8': Keyboard.press('8'); break;
    case '9': Keyboard.press('9'); break;
    case KEY_F1: Keyboard.press(KEY_F1); break;
    case KEY_F2: Keyboard.press(KEY_F2); break;
    case KEY_F3: Keyboard.press(KEY_F3); break;
    case KEY_F4: Keyboard.press(KEY_F4); break;
    case KEY_F5: Keyboard.press(KEY_F5); break;
    case KEY_F6: Keyboard.press(KEY_F6); break;
    case KEY_F7: Keyboard.press(KEY_F7); break;
    case KEY_F8: Keyboard.press(KEY_F8); break;
    case KEY_F9: Keyboard.press(KEY_F9); break;
    case KEY_F10: Keyboard.press(KEY_F10); break;
    case KEY_F11: Keyboard.press(KEY_F11); break;
    case KEY_F12: Keyboard.press(KEY_F12); break;
    case KEY_F13: Keyboard.press(KEY_F13); break;
    case KEY_F14: Keyboard.press(KEY_F14); break;
    case KEY_F15: Keyboard.press(KEY_F15); break;
    case KEY_F16: Keyboard.press(KEY_F16); break;
    case KEY_F17: Keyboard.press(KEY_F17); break;
    case KEY_F18: Keyboard.press(KEY_F18); break;
    case KEY_F19: Keyboard.press(KEY_F19); break;
    case KEY_F20: Keyboard.press(KEY_F20); break;
    case KEY_F21: Keyboard.press(KEY_F21); break;
    case KEY_F22: Keyboard.press(KEY_F22); break;
    case KEY_F23: Keyboard.press(KEY_F23); break;
    case KEY_F24: Keyboard.press(KEY_F24); break;
    case KEY_LEFT_CTRL: Keyboard.press(KEY_LEFT_CTRL); break;
    case KEY_LEFT_SHIFT: Keyboard.press(KEY_LEFT_SHIFT); break;
    case KEY_LEFT_ALT: Keyboard.press(KEY_LEFT_ALT); break;
    case KEY_LEFT_GUI: Keyboard.press(KEY_LEFT_GUI); break;  // Windows/Command
    case KEY_RIGHT_CTRL: Keyboard.press(KEY_RIGHT_CTRL); break;
    case KEY_RIGHT_SHIFT: Keyboard.press(KEY_RIGHT_SHIFT); break;
    case KEY_RIGHT_ALT: Keyboard.press(KEY_RIGHT_ALT); break;  // Alt Gr
    case KEY_RIGHT_GUI: Keyboard.press(KEY_RIGHT_GUI); break;
    case KEY_BACKSPACE: Keyboard.press(KEY_BACKSPACE); break;
    case KEY_TAB: Keyboard.press(KEY_TAB); break;
    case KEY_RETURN: Keyboard.press(KEY_RETURN); break;  // Enter
    case KEY_ESC: Keyboard.press(KEY_ESC); break;
    case KEY_INSERT: Keyboard.press(KEY_INSERT); break;
    case KEY_DELETE: Keyboard.press(KEY_DELETE); break;
    case KEY_PAGE_UP: Keyboard.press(KEY_PAGE_UP); break;
    case KEY_PAGE_DOWN: Keyboard.press(KEY_PAGE_DOWN); break;
    case KEY_HOME: Keyboard.press(KEY_HOME); break;
    case KEY_END: Keyboard.press(KEY_END); break;
    case KEY_CAPS_LOCK:
      Serial.println(">>> DEBUG: Pressing CAPS LOCK via workaround");
      Keyboard.press(KEY_CAPS_LOCK);
      break;
    case KEY_PRINT_SCREEN: Keyboard.press(KEY_PRINT_SCREEN); break;
    case KEY_SCROLL_LOCK:
      Serial.println(">>> DEBUG: Pressing SCROLL LOCK via workaround");
      Keyboard.press(KEY_SCROLL_LOCK);
      break;
    case KEY_UP_ARROW: Keyboard.press(KEY_UP_ARROW); break;
    case KEY_DOWN_ARROW: Keyboard.press(KEY_DOWN_ARROW); break;
    case KEY_LEFT_ARROW: Keyboard.press(KEY_LEFT_ARROW); break;
    case KEY_RIGHT_ARROW: Keyboard.press(KEY_RIGHT_ARROW); break;
    case KEY_NUM_LOCK:
      Serial.println(">>> DEBUG: Pressing NUM LOCK via workaround");
      Keyboard.press(KEY_NUM_LOCK);
      break;
    case KEY_KP_SLASH: Keyboard.press(KEY_KP_SLASH); break;
    case KEY_KP_ASTERISK: Keyboard.press(KEY_KP_ASTERISK); break;
    case KEY_KP_MINUS: Keyboard.press(KEY_KP_MINUS); break;
    case KEY_KP_PLUS: Keyboard.press(KEY_KP_PLUS); break;
    case KEY_KP_ENTER: Keyboard.press(KEY_KP_ENTER); break;
    case KEY_KP_1: Keyboard.press(KEY_KP_1); break;
    case KEY_KP_2: Keyboard.press(KEY_KP_2); break;
    case KEY_KP_3: Keyboard.press(KEY_KP_3); break;
    case KEY_KP_4: Keyboard.press(KEY_KP_4); break;
    case KEY_KP_5: Keyboard.press(KEY_KP_5); break;
    case KEY_KP_6: Keyboard.press(KEY_KP_6); break;
    case KEY_KP_7: Keyboard.press(KEY_KP_7); break;
    case KEY_KP_8: Keyboard.press(KEY_KP_8); break;
    case KEY_KP_9: Keyboard.press(KEY_KP_9); break;
    case KEY_KP_0: Keyboard.press(KEY_KP_0); break;
    case KEY_KP_DOT: Keyboard.press(KEY_KP_DOT); break;
    case KEY_SPACE: Keyboard.press(KEY_SPACE); break;


    default:
      Serial.printf("Warning: Unhandled key press in workaround: 0x%02X\n", key);
      break;
  }
}
// 使用 switch 语句根据按键码调用对应的 Keyboard.release(常量)
void releaseMappedKey(uint8_t key) {
  // 如果 key 是 0 (未映射)，则不执行任何操作
  if (key == 0) return;

  Serial.printf("<-- Workaround Release: 0x%02X\n", key);
  switch (key) {
    case 'a':
    case 'A': Keyboard.release('a'); break;
    case 'b':
    case 'B': Keyboard.release('b'); break;
    case 'c':
    case 'C': Keyboard.release('c'); break;
    case 'd':
    case 'D': Keyboard.release('d'); break;
    case 'e':
    case 'E': Keyboard.release('e'); break;
    case 'f':
    case 'F': Keyboard.release('f'); break;
    case 'g':
    case 'G': Keyboard.release('g'); break;
    case 'h':
    case 'H': Keyboard.release('h'); break;
    case 'i':
    case 'I': Keyboard.release('i'); break;
    case 'j':
    case 'J': Keyboard.release('j'); break;
    case 'k':
    case 'K': Keyboard.release('k'); break;
    case 'l':
    case 'L': Keyboard.release('l'); break;
    case 'm':
    case 'M': Keyboard.release('m'); break;
    case 'n':
    case 'N': Keyboard.release('n'); break;
    case 'o':
    case 'O': Keyboard.release('o'); break;
    case 'p':
    case 'P': Keyboard.release('p'); break;
    case 'q':
    case 'Q': Keyboard.release('q'); break;
    case 'r':
    case 'R': Keyboard.release('r'); break;
    case 's':
    case 'S': Keyboard.release('s'); break;
    case 't':
    case 'T': Keyboard.release('t'); break;
    case 'u':
    case 'U': Keyboard.release('u'); break;
    case 'v':
    case 'V': Keyboard.release('v'); break;
    case 'w':
    case 'W': Keyboard.release('w'); break;
    case 'x':
    case 'X': Keyboard.release('x'); break;
    case 'y':
    case 'Y': Keyboard.release('y'); break;
    case 'z':
    case 'Z': Keyboard.release('z'); break;
    case '0': Keyboard.release('0'); break;
    case '1': Keyboard.release('1'); break;
    case '2': Keyboard.release('2'); break;
    case '3': Keyboard.release('3'); break;
    case '4': Keyboard.release('4'); break;
    case '5': Keyboard.release('5'); break;
    case '6': Keyboard.release('6'); break;
    case '7': Keyboard.release('7'); break;
    case '8': Keyboard.release('8'); break;
    case '9': Keyboard.release('9'); break;
    case KEY_F1: Keyboard.release(KEY_F1); break;
    case KEY_F2: Keyboard.release(KEY_F2); break;
    case KEY_F3: Keyboard.release(KEY_F3); break;
    case KEY_F4: Keyboard.release(KEY_F4); break;
    case KEY_F5: Keyboard.release(KEY_F5); break;
    case KEY_F6: Keyboard.release(KEY_F6); break;
    case KEY_F7: Keyboard.release(KEY_F7); break;
    case KEY_F8: Keyboard.release(KEY_F8); break;
    case KEY_F9: Keyboard.release(KEY_F9); break;
    case KEY_F10: Keyboard.release(KEY_F10); break;
    case KEY_F11: Keyboard.release(KEY_F11); break;
    case KEY_F12: Keyboard.release(KEY_F12); break;
    case KEY_F13: Keyboard.release(KEY_F13); break;
    case KEY_F14: Keyboard.release(KEY_F14); break;
    case KEY_F15: Keyboard.release(KEY_F15); break;
    case KEY_F16: Keyboard.release(KEY_F16); break;
    case KEY_F17: Keyboard.release(KEY_F17); break;
    case KEY_F18: Keyboard.release(KEY_F18); break;
    case KEY_F19: Keyboard.release(KEY_F19); break;
    case KEY_F20: Keyboard.release(KEY_F20); break;
    case KEY_F21: Keyboard.release(KEY_F21); break;
    case KEY_F22: Keyboard.release(KEY_F22); break;
    case KEY_F23: Keyboard.release(KEY_F23); break;
    case KEY_F24: Keyboard.release(KEY_F24); break;
    case KEY_LEFT_CTRL: Keyboard.release(KEY_LEFT_CTRL); break;
    case KEY_LEFT_SHIFT: Keyboard.release(KEY_LEFT_SHIFT); break;
    case KEY_LEFT_ALT: Keyboard.release(KEY_LEFT_ALT); break;
    case KEY_LEFT_GUI: Keyboard.release(KEY_LEFT_GUI); break;
    case KEY_RIGHT_CTRL: Keyboard.release(KEY_RIGHT_CTRL); break;
    case KEY_RIGHT_SHIFT: Keyboard.release(KEY_RIGHT_SHIFT); break;
    case KEY_RIGHT_ALT: Keyboard.release(KEY_RIGHT_ALT); break;
    case KEY_RIGHT_GUI: Keyboard.release(KEY_RIGHT_GUI); break;
    case KEY_BACKSPACE: Keyboard.release(KEY_BACKSPACE); break;
    case KEY_TAB: Keyboard.release(KEY_TAB); break;
    case KEY_RETURN: Keyboard.release(KEY_RETURN); break;
    case KEY_ESC: Keyboard.release(KEY_ESC); break;
    case KEY_INSERT: Keyboard.release(KEY_INSERT); break;
    case KEY_DELETE: Keyboard.release(KEY_DELETE); break;
    case KEY_PAGE_UP: Keyboard.release(KEY_PAGE_UP); break;
    case KEY_PAGE_DOWN: Keyboard.release(KEY_PAGE_DOWN); break;
    case KEY_HOME: Keyboard.release(KEY_HOME); break;
    case KEY_END: Keyboard.release(KEY_END); break;
    case KEY_CAPS_LOCK:
      Serial.println("<<< DEBUG: Releasing CAPS LOCK via workaround");  // Keep debug log
      Keyboard.release(KEY_CAPS_LOCK);
      break;
    case KEY_PRINT_SCREEN: Keyboard.release(KEY_PRINT_SCREEN); break;
    case KEY_SCROLL_LOCK:
      Serial.println("<<< DEBUG: Releasing SCROLL LOCK via workaround");
      Keyboard.release(KEY_SCROLL_LOCK);
      break;
    case KEY_UP_ARROW: Keyboard.release(KEY_UP_ARROW); break;
    case KEY_DOWN_ARROW: Keyboard.release(KEY_DOWN_ARROW); break;
    case KEY_LEFT_ARROW: Keyboard.release(KEY_LEFT_ARROW); break;
    case KEY_RIGHT_ARROW: Keyboard.release(KEY_RIGHT_ARROW); break;
    case KEY_NUM_LOCK:
      Serial.println("<<< DEBUG: Releasing NUM LOCK via workaround");
      Keyboard.release(KEY_NUM_LOCK);
      break;
    case KEY_KP_SLASH: Keyboard.release(KEY_KP_SLASH); break;
    case KEY_KP_ASTERISK: Keyboard.release(KEY_KP_ASTERISK); break;
    case KEY_KP_MINUS: Keyboard.release(KEY_KP_MINUS); break;
    case KEY_KP_PLUS: Keyboard.release(KEY_KP_PLUS); break;
    case KEY_KP_ENTER: Keyboard.release(KEY_KP_ENTER); break;
    case KEY_KP_1: Keyboard.release(KEY_KP_1); break;
    case KEY_KP_2: Keyboard.release(KEY_KP_2); break;
    case KEY_KP_3: Keyboard.release(KEY_KP_3); break;
    case KEY_KP_4: Keyboard.release(KEY_KP_4); break;
    case KEY_KP_5: Keyboard.release(KEY_KP_5); break;
    case KEY_KP_6: Keyboard.release(KEY_KP_6); break;
    case KEY_KP_7: Keyboard.release(KEY_KP_7); break;
    case KEY_KP_8: Keyboard.release(KEY_KP_8); break;
    case KEY_KP_9: Keyboard.release(KEY_KP_9); break;
    case KEY_KP_0: Keyboard.release(KEY_KP_0); break;
    case KEY_KP_DOT: Keyboard.release(KEY_KP_DOT); break;
    case KEY_SPACE: Keyboard.release(KEY_SPACE); break;

    default:
      Serial.printf("Warning: Unhandled key release in workaround: 0x%02X\n", key);
      break;
  }
}


// 其他辅助函数
void setupDefaultProfiles() {
  // Profile 1 (默认)
  profiles[0].joystickButton = KEY_SPACE;
  profiles[0].customButtons[0] = KEY_F1;
  profiles[0].customButtons[1] = KEY_F2;
  profiles[0].customButtons[2] = KEY_F3;
  profiles[0].customButtons[3] = KEY_F4;
  profiles[0].customButtons[4] = '1';
  profiles[0].customButtons[5] = '2';
  profiles[0].customButtons[6] = '3';
  profiles[0].customButtons[7] = 'q';
  profiles[0].customButtons[8] = 'e';
  // Profile 2
  profiles[1].joystickButton = KEY_LEFT_CTRL;
  profiles[1].customButtons[0] = 'r';
  profiles[1].customButtons[1] = 'f';
  profiles[1].customButtons[2] = 'g';
  profiles[1].customButtons[3] = 'c';
  profiles[1].customButtons[4] = 'z';
  profiles[1].customButtons[5] = 'x';
  profiles[1].customButtons[6] = KEY_TAB;
  profiles[1].customButtons[7] = KEY_LEFT_SHIFT;
  profiles[1].customButtons[8] = KEY_RETURN;
  // Profile 3
  profiles[2].joystickButton = KEY_SPACE;
  profiles[2].customButtons[0] = KEY_UP_ARROW;
  profiles[2].customButtons[1] = KEY_DOWN_ARROW;
  profiles[2].customButtons[2] = KEY_LEFT_ARROW;
  profiles[2].customButtons[3] = KEY_RIGHT_ARROW;
  profiles[2].customButtons[4] = KEY_HOME;
  profiles[2].customButtons[5] = KEY_END;
  profiles[2].customButtons[6] = KEY_PAGE_UP;
  profiles[2].customButtons[7] = KEY_PAGE_DOWN;
  profiles[2].customButtons[8] = KEY_ESC;
  // Profile 4
  profiles[3] = profiles[1];
  // Profile 5
  profiles[4] = profiles[0];
}

void loadProfile(int profileIndex) {
  if (profileIndex < 0 || profileIndex >= NUM_PROFILES) return;
  Serial.printf(" 加载 Profile %d 的映射...\n", profileIndex + 1);
  String keyPrefix = "p" + String(profileIndex) + "_";
  String joyBtnKey = keyPrefix + "jb";
  profiles[profileIndex].joystickButton = preferences.getUChar(joyBtnKey.c_str(), profiles[profileIndex].joystickButton);
  for (int i = 0; i < NUM_CUSTOM_BUTTONS; i++) {
    String btnKey = keyPrefix + "b" + String(i);
    profiles[profileIndex].customButtons[i] = preferences.getUChar(btnKey.c_str(), profiles[profileIndex].customButtons[i]);
  }
  if (!inConfigMode) {
    joystickButtonState.mappedKey = profiles[profileIndex].joystickButton;
    joystickButtonState.isPressed = false;
    for (int i = 0; i < NUM_CUSTOM_BUTTONS; i++) {
      customButtonStates[i].mappedKey = profiles[profileIndex].customButtons[i];
      customButtonStates[i].isPressed = false;
    }
  }
  Serial.printf(" Profile %d 加载完成.\n", profileIndex + 1);
}

void saveProfileIndex(int profileIndex) {
  preferences.putInt(PROFILE_INDEX_KEY, profileIndex);
}

void saveAllProfilesToNVS() {
  for (int p = 0; p < NUM_PROFILES; ++p) {
    String keyPrefix = "p" + String(p) + "_";
    preferences.putUChar((keyPrefix + "jb").c_str(), profiles[p].joystickButton);
    for (int b = 0; b < NUM_CUSTOM_BUTTONS; ++b) {
      String btnKey = keyPrefix + "b" + String(b);
      preferences.putUChar(btnKey.c_str(), profiles[p].customButtons[b]);
    }
  }
  Serial.println("NVS 写入操作完成.");
}

void readAndUpdateButton(ButtonState &btnState) {
  bool reading = digitalRead(btnState.pin);
  if (reading != btnState.lastState) {
    btnState.lastDebounceTime = millis();
  }
  if ((millis() - btnState.lastDebounceTime) > debounceDelay) {
    if (reading != btnState.currentState) {
      btnState.currentState = reading;
    }
  }
  btnState.lastState = reading;
}

void handleJoystickMovement(int xValue, int yValue) {
  // X-axis (A/D)
  if (xValue < ADC_THRESHOLD_LOW) {  // Left
    if (!a_pressed) {
      Keyboard.press('a');
      a_pressed = true;
    }
    if (d_pressed) {
      Keyboard.release('d');
      d_pressed = false;
    }
  } else if (xValue > ADC_THRESHOLD_HIGH) {  // Right
    if (!d_pressed) {
      Keyboard.press('d');
      d_pressed = true;
    }
    if (a_pressed) {
      Keyboard.release('a');
      a_pressed = false;
    }
  } else {
    if (a_pressed) {
      Keyboard.release('a');
      a_pressed = false;
    }
    if (d_pressed) {
      Keyboard.release('d');
      d_pressed = false;
    }
  }
  // Y-axis (W/S)
  if (yValue > ADC_THRESHOLD_HIGH) {  // Up -> W
    if (!w_pressed) {
      Keyboard.press('w');
      w_pressed = true;
    }
    if (s_pressed) {
      Keyboard.release('s');
      s_pressed = false;
    }
  } else if (yValue < ADC_THRESHOLD_LOW) {  // Down -> S
    if (!s_pressed) {
      Keyboard.press('s');
      s_pressed = true;
    }
    if (w_pressed) {
      Keyboard.release('w');
      w_pressed = false;
    }
  } else {
    if (w_pressed) {
      Keyboard.release('w');
      w_pressed = false;
    }
    if (s_pressed) {
      Keyboard.release('s');
      s_pressed = false;
    }
  }
}

void handleButtonPress(ButtonState &btnState) {
  if (btnState.pin == PROFILE_BUTTON_PIN) return;
  bool physicallyPressed = (btnState.currentState == LOW);
  if (physicallyPressed && !btnState.isPressed) {
    pressMappedKey(btnState.mappedKey);
    btnState.isPressed = true;
  } else if (!physicallyPressed && btnState.isPressed) {
    releaseMappedKey(btnState.mappedKey);
    btnState.isPressed = false;
  }
}

// 新的非阻塞闪烁函数
// 开始非阻塞闪烁序列
void startProfileBlink(int times) {
  Serial.printf(">>> DEBUG: startProfileBlink called with times = %d (for Profile %d)\n", times, currentProfile + 1);

  if (times <= 0) {
    isBlinkingProfile = false;
    digitalWrite(LED_PIN, LOW);
    currentLedState = LOW;
    blinksToDo = 0;
    return;
  }
  blinksToDo = times;
  isBlinkingProfile = true;
  currentLedState = HIGH;
  digitalWrite(LED_PIN, currentLedState);
  lastBlinkChangeTime = millis();
}

// 在主循环中调用，处理非阻塞闪烁的状态更新
void handleProfileBlink() {
  // 如果当前没有在执行 Profile 闪烁，则直接返回
  if (!isBlinkingProfile) {
    return;
  }

  unsigned long currentTime = millis();
  // 根据当前 LED 状态决定需要等待的时长
  unsigned long duration = currentLedState ? blinkOnDuration : blinkOffDuration;

  // 检查是否到达状态切换的时间点
  if (currentTime - lastBlinkChangeTime >= duration) {
    currentLedState = !currentLedState;      // 切换 LED 状态 (亮->灭 或 灭->亮)
    digitalWrite(LED_PIN, currentLedState);  // 更新 LED 物理状态
    lastBlinkChangeTime = currentTime;       // 记录状态改变时间

    // 如果刚刚完成了一次“灭灯”过程，说明一次完整的闪烁结束
    if (!currentLedState) {  // Just turned OFF
      blinksToDo--;          // 减少剩余次数
      // 如果所有次数都完成了
      if (blinksToDo <= 0) {
        isBlinkingProfile = false;  // 标记闪烁结束
      }
    }
  }
}

// 用于 Setup 中简单提示的阻塞式闪烁
void blinkLedBlocking(int times, int onDuration /*= 100*/, int offDuration) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(onDuration);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) {
      delay(offDuration);
    }
  }
}

// 配置模式辅助函数
void showHelp() {
  Serial.println("\n--- 配置模式帮助 ---");
  Serial.println("命令格式 (不区分大小写，但按键名称建议按需区分):");
  Serial.println(" help - 显示此帮助信息");
  Serial.println(" show <profile> - 显示指定 Profile (1-5) 的按键映射 (显示为16进制码)");
  Serial.println(" P<p> <target> <key> - 设置按键映射");
  Serial.println("   <p>: Profile 编号 (1-5)");
  Serial.println("   <target>: 'JOY' (摇杆按钮) 或 'B<index>' (按钮 1-" + String(NUM_CUSTOM_BUTTONS) + ")");
  Serial.println("   <key>: 按键名称 (见下) 或 16 进制代码 (0xNN)");
  Serial.println(" save - 将当前所有 Profiles 的映射永久保存到闪存");
  Serial.println(" reset - 将内存中的所有映射重置为默认值 (需 'save' 保存)");
  Serial.println(" exit - 保存并退出配置模式 (设备将自动重启)");
  Serial.println("\n可用按键名称示例 (参考 parseKeyCode 函数):");
  Serial.println(" 字母/数字: a, B, ..., z, 0, 1, ..., 9");
  Serial.println(" 功能键: F1-F24");
  Serial.println(" 特殊键: SPACE, ENTER/RETURN, ESC, TAB, BACKSPACE, DELETE, INSERT");
  Serial.println("        HOME, END, PAGEUP, PAGEDOWN, PRINTSCREEN");
  Serial.println("        CAPSLOCK, NUMLOCK, SCROLLLOCK");
  Serial.println(" 修饰键: LCTRL, LSHIFT, LALT, LGUI, RCTRL, RSHIFT, RALT, RGUI");
  Serial.println(" 方向键: UP, DOWN, LEFT, RIGHT");
  Serial.println(" 小键盘: KP0-KP9, KP_SLASH, KP_ASTERISK, KP_MINUS, KP_PLUS, KP_ENTER, KP_DOT");
  Serial.println("--------------------\n");
}

void showProfile(int p) {
  if (p < 0 || p >= NUM_PROFILES) return;
  Serial.printf("\n--- Profile %d 映射 ---\n", p + 1);
  Serial.printf(" 摇杆按钮 (JOY): 0x%02X\n", profiles[p].joystickButton);
  for (int i = 0; i < NUM_CUSTOM_BUTTONS; ++i) {
    Serial.printf(" 自定义按钮 %d (B%d): 0x%02X\n", i + 1, i + 1, profiles[p].customButtons[i]);
  }
  Serial.println("--------------------");
}

bool setKeyMap(int p, String targetType, int targetIndex, String keyCodeStr) {
  if (p < 0 || p >= NUM_PROFILES) return false;
  uint8_t keyCode = parseKeyCode(keyCodeStr);
  if (keyCode == 0 && !(keyCodeStr == "0" || keyCodeStr.equalsIgnoreCase("KP0") || keyCodeStr.equalsIgnoreCase("0X00"))) {
    Serial.print("错误: 无法解析按键 '");
    Serial.print(keyCodeStr);
    Serial.println("'.");
    return false;
  }
  if (targetType == "joy") {
    profiles[p].joystickButton = keyCode;
    Serial.printf("内存更新: Profile %d JOY 设置为 0x%02X (%s)\n", p + 1, keyCode, keyCodeStr.c_str());
    return true;
  } else if (targetType == "btn") {
    if (targetIndex >= 1 && targetIndex <= NUM_CUSTOM_BUTTONS) {
      profiles[p].customButtons[targetIndex - 1] = keyCode;
      Serial.printf("内存更新: Profile %d B%d 设置为 0x%02X (%s)\n", p + 1, targetIndex, keyCode, keyCodeStr.c_str());
      return true;
    } else {
      return false;
    }
  } else {
    return false;
  }
}

uint8_t parseKeyCode(String keyName) {
  keyName.toUpperCase();
  if (keyName.startsWith("0X")) {
    char *endptr;
    long code = strtol(keyName.c_str(), &endptr, 16);
    if (*endptr == '\0' && code >= 0 && code <= 0xFF) return (uint8_t)code;
    else return 0;
  }
  if (keyName.length() == 1) {
    char c = keyName.charAt(0);
    if (c >= 'A' && c <= 'Z') return (uint8_t)tolower(c);
    if (c >= '0' && c <= '9') return (uint8_t)c;
  }
  if (keyName == "SPACE") return KEY_SPACE;
  if (keyName == "ENTER" || keyName == "RETURN") return KEY_RETURN;
  if (keyName == "ESC" || keyName == "ESCAPE") return KEY_ESC;
  if (keyName == "TAB") return KEY_TAB;
  if (keyName == "BACKSPACE") return KEY_BACKSPACE;
  if (keyName == "DEL" || keyName == "DELETE") return KEY_DELETE;
  if (keyName == "INS" || keyName == "INSERT") return KEY_INSERT;
  if (keyName == "UP") return KEY_UP_ARROW;
  if (keyName == "DOWN") return KEY_DOWN_ARROW;
  if (keyName == "LEFT") return KEY_LEFT_ARROW;
  if (keyName == "RIGHT") return KEY_RIGHT_ARROW;
  if (keyName == "HOME") return KEY_HOME;
  if (keyName == "END") return KEY_END;
  if (keyName == "PAGEUP" || keyName == "PGUP") return KEY_PAGE_UP;
  if (keyName == "PAGEDOWN" || keyName == "PGDN") return KEY_PAGE_DOWN;
  if (keyName == "PRINTSCREEN" || keyName == "PRTSC") return KEY_PRINT_SCREEN;
  if (keyName == "CAPSLOCK" || keyName == "CAPS") return KEY_CAPS_LOCK;
  if (keyName == "NUMLOCK") return KEY_NUM_LOCK;
  if (keyName == "SCROLLLOCK") return KEY_SCROLL_LOCK;
  if (keyName == "LCTRL" || keyName == "LEFTCTRL") return KEY_LEFT_CTRL;
  if (keyName == "LSHIFT" || keyName == "LEFTSHIFT") return KEY_LEFT_SHIFT;
  if (keyName == "LALT" || keyName == "LEFTALT") return KEY_LEFT_ALT;
  if (keyName == "LGUI" || keyName == "LEFTGUI" || keyName == "LWIN" || keyName == "LEFTWINDOWS") return KEY_LEFT_GUI;
  if (keyName == "RCTRL" || keyName == "RIGHTCTRL") return KEY_RIGHT_CTRL;
  if (keyName == "RSHIFT" || keyName == "RIGHTSHIFT") return KEY_RIGHT_SHIFT;
  if (keyName == "RALT" || keyName == "RIGHTALT") return KEY_RIGHT_ALT;
  if (keyName == "RGUI" || keyName == "RIGHTGUI" || keyName == "RWIN" || keyName == "RIGHTWINDOWS") return KEY_RIGHT_GUI;
  if (keyName.startsWith("F")) {
    int fNum = keyName.substring(1).toInt();
    if (fNum >= 1 && fNum <= 12) return KEY_F1 + (fNum - 1);
    if (fNum >= 13 && fNum <= 24) return KEY_F13 + (fNum - 13);
  }
  if (keyName.startsWith("KP")) {
    if (keyName == "KP_SLASH" || keyName == "KPSLASH") return KEY_KP_SLASH;
    if (keyName == "KP_ASTERISK" || keyName == "KPASTERISK" || keyName == "KPSTAR") return KEY_KP_ASTERISK;
    if (keyName == "KP_MINUS" || keyName == "KPMINUS") return KEY_KP_MINUS;
    if (keyName == "KP_PLUS" || keyName == "KPPLUS") return KEY_KP_PLUS;
    if (keyName == "KP_ENTER" || keyName == "KPENTER") return KEY_KP_ENTER;
    if (keyName == "KP_DOT" || keyName == "KPDOT" || keyName == "KPDEL") return KEY_KP_DOT;
    if (keyName == "KP1") return KEY_KP_1;
    if (keyName == "KP2") return KEY_KP_2;
    if (keyName == "KP3") return KEY_KP_3;
    if (keyName == "KP4") return KEY_KP_4;
    if (keyName == "KP5") return KEY_KP_5;
    if (keyName == "KP6") return KEY_KP_6;
    if (keyName == "KP7") return KEY_KP_7;
    if (keyName == "KP8") return KEY_KP_8;
    if (keyName == "KP9") return KEY_KP_9;
    if (keyName == "KP0") return KEY_KP_0;
  }
  return 0;
}