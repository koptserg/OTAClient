### Добавить устройство в сеть:
1. Нажмите и удерживайте кнопку S2 в течении 3 секунд
2. Подождите, в случае успешного подключения светодиод D1 устройства мигнет 5 раз.
3. Если подключение не удалось, светодиод D1 устройства мигнет 3 раза.

### Отправить отчет по батарейке:
1. Коротко нажмите кнопку S2, устройство мигнет светодиодом D1.

### Отправить команду переключения:
1. Коротко нажмите кнопку S1, устройство мигнет светодиодом D1.

### Удалить устройство из сети:
1. Удерживайте кнопку S2 в течение 10 секунд, это сбросит устройство в состояние FN (заводское установки).

### Аппаратное обеспечение
![](/images/photo_2025-03-05_12-22-52.jpg)

1. Плата отладки СС2530 ChdTech.
2. M25PE20-VMN6TP, Флэш-память 2Мбит 75МГц 8SO 
3. MISO(2) - P1.7, MOSI(5) - P1.6, SCLK(6) - P1.5, CS(1) - P1.3, RESET(7) - P1.1, VCC(8,3), GND(4)

### Инструкция по добавлению в ваш проект ОТА client с внешней памятью
1. Создаем файл прошивки своего проекта EndDeviceEB-OTAClient.hex с настройкой ОТА client и файл образа загрузки по OTA 5678-1234-0000ABCD.zigbee 
2. Создаем файл прошивки проекта OTA-Boot Boot.hex
3. Создание объединенного шестнадцатеричного файла, подходящий для TI Flash Programmer.

### 1. Создаем файл прошивки своего проекта EndDeviceEB-OTAClient.hex с настройкой ОТА client и файл образа загрузки по OTA 5678-1234-0000ABCD.zigbee 
1.1. Открываем свой проект в IAR. 

![](/images/Screenshot_2239.jpg)

1.2. Переходим Project>Options, категория Linker 

1.2.1. Закладка Config (файл управления компоновщиком ota.xcl)

![](/images/Screenshot_2240.jpg)

1.2.2. Закладка Output (задаем имя выходного файл EndDeviceEB-OTAClient.hex в формате hex)

![](/images/Screenshot_2241.jpg)

1.2.3. Закладка Extra Output (файл EndDeviceEB-OTAClient.sim нужен для формирования образа OTA 5678-1234-0000ABCD.zigbee)

![](/images/Screenshot_2242.jpg)

1.3. Категория Build Action

1.3.1. Post-Build command line (записываем команду формирования образа OTA 5678-1234-0000ABCD.zigbee)

"$PROJ_DIR$\..\..\..\..\tools\OTA\OtaConverter\Release\OtaConverter.exe" "$PROJ_DIR$\OTACLIENT_CHDTECH\Exe\EndDeviceEB-OTAClient.sim" -o"$PROJ_DIR$\OTACLIENT_CHDTECH\Exe" -t0x1234 -m0x5678 -v0000ABCD -pCC2530DB

![](/images/Screenshot_2243.jpg)

1.4. Категория C/C++Compiler

1.4.1. Закладка Preprocessor прописываем путь для поиска OTA\Source и preinclude.h

![](/images/Screenshot_2246.jpg) 

1.4.2. Изменяем файл preinclude.h

![](/images/Screenshot_2247.jpg) 

1.5. Файл управления компоновщиком ota.xcl

1.5.1. Добавляем в проект ota.xcl, открываем и убираем комментарии // на двух отмеченных строчках 

![](/images/Screenshot_2245.jpg)

1.6. Добавляем в проект файлы исходников OTA

1.6.1. Файлы HAL_OTA hal_ota.h и hal_ota.с

![](/images/Screenshot_2248.jpg)

1.6.2. Файлы ZCL_OTA zcl_ota.h и zcl_ota.c

![](/images/Screenshot_2249.jpg)

1.7. Изменяем файлы исходников проекта

1.7.1. hal_board_cfg.h добавляем SPI для внешней флеш-памяти

![](/images/Screenshot_2250.jpg)

1.7.2. Osal_App.c

1.7.2.1. Добавляем цикл zclOTA

![](/images/Screenshot_2251.jpg)

1.7.2.2. Добавляем задачу zclOTA

![](/images/Screenshot_2252.jpg)

1.7.3. zcl_app.c

1.7.3.1. Подключаем библиотеки zcl_ota.h и hal_ota.h

![](/images/Screenshot_2253.jpg)

1.7.3.2. Устанавливаем Poll rate

![](/images/Screenshot_2254.jpg)

1.7.3.3. Функция обработки сообщений

![](/images/Screenshot_2255.jpg)

1.7.3.3. Регистрация для событий обратного вызова от ZCL OTA

![](/images/Screenshot_2256.jpg)

1.7.3.3. Сработка индикатора ZCL_OTA_CALLBACK_IND

![](/images/Screenshot_2257.jpg)

1.7.3.3. Обработчик сообщений ZCL OTA

![](/images/Screenshot_2258.jpg)

1.8. Собрать проект Rebuild All

![](/images/Screenshot_2259.jpg)

### 2. Создаем файл прошивки проекта OTA-Boot Boot.hex

2.1. Открываем проект OTA-Boot в IAR
C:\Texas Instruments\Z-Stack 3.0.2\Projects\zstack\OTA\Boot\CC2530DB\Boot.eww

![](/images/Screenshot_2260.jpg)

2.2. Переходим Project>Options, категория Linker 

2.2.1. Закладка Output (установить Allow C-SPY-specific extra output file)

![](/images/Screenshot_2261.jpg)

2.2.2. Закладка Extra Output (Boot.hex в шеснатеричном формате)

![](/images/Screenshot_2262.jpg)

2.3. Собрать проект Rebuild All (нам нужен Boot.hex)

![](/images/Screenshot_2263.jpg)

### 3. Создание объединенного шестнадцатеричного файла, подходящий для TI Flash Programmer.

3.1. Используйте любой текстовый редактор, чтобы открыть hex-файл приложения, созданный выше: 
- C:\Texas Instruments\Z-Stack 3.0.2\Projects\zstack\HomeAutomation\OTAClient\CC2530DB\OTACLIENT_CHDTECH\Exe\EndDeviceEB-OTAClient.hex

3.2. Удалите эту первую строку из файла:
- :020000040000FA

3.3. В отдельном окне текстового редактора откройте hex-файл приложения OTA Boot, созданный выше: 
- C:\Texas Instruments\Z-Stack 3.0.2\Projects\zstack\OTA\Boot\CC2530DB\OTA-Boot\Exe\Boot.hex

3.4. Удалите последние две строки из файла Boot.hex. Они должны выглядеть следующим образом:
- :0400000500000738B8
- :00000001FF

3.5. Скопируйте отредактированное содержимое hex-файла приложения OTA Boot Boot.hex в начало файла кода приложения EndDeviceEB-OTAClient.hex и сохраните его.

![](/images/Screenshot_2264.jpg)

3.6. Используйте программатор SmartRF для установки отредактированного шестнадцатеричного образа в SoC CC2530.