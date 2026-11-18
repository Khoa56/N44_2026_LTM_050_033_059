# Giám sát ESP32 và bay mô phỏng PX4 SITL

ESP32-C3 đo nhiệt độ và độ ẩm, hiển thị trên OLED và gửi về máy tính. Chương trình giám sát kết hợp các số đo này với chế độ bay, vị trí và độ cao từ PX4. Chương trình kịch bản bay điều khiển UAV mô phỏng qua bốn điểm rồi quay về hạ cánh. Hai chương trình cùng build từ thư mục gốc, dùng chung MAVSDK C++ 3.17.5.

| Phần | Vị trí | Cổng tích hợp |
| --- | --- | --- |
| Tuấn · ESP32-C3, DHT11, OLED và gửi lại khi thiếu ACK | [Firmware PlatformIO](ESP32-C3-LTMProject-Tuan/) | Gửi đến máy giám sát UDP `8080` |
| Khoa · Nhận ESP32, trả ACK, hiển thị trạng thái và ghi CSV | [Mã giám sát C++](src/) | Nhận ESP32 UDP `8080` và MAVLink UDP `14600` |
| Sơn · PX4 SITL và kịch bản bay | [Mã bay và script mô phỏng](Khoi2_UAV_PX4_Son/) | Nhận MAVLink UDP `14540` trên máy chạy SITL |

## 1. Chuẩn bị máy tính

### Ubuntu 22.04 x86-64 hoặc Ubuntu 22.04 trên WSL2

Trên Windows, nếu cần Ubuntu cho SITL, mở PowerShell với quyền quản trị viên và chạy `wsl --install -d Ubuntu-22.04`. Sau đó mở Ubuntu và chuyển đến thư mục dự án. Trong WSL2, ổ `C:` của Windows được truy cập qua `/mnt/c`.

```bash
cd '/mnt/c/Users/TEN_NGUOI_DUNG/Downloads/esp32-px4'
```

Trong Ubuntu, cài môi trường jMAVSim/PX4 v1.14.3 và MAVSDK v3.17.5, rồi build hai chương trình.

```bash
bash Khoi2_UAV_PX4_Son/scripts/2_setup_px4_mavsdk_ubuntu.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Script dùng `$HOME/PX4-Autopilot` hoặc đường dẫn đặt bằng biến `PX4_DIR`. Nếu đã có một checkout PX4 phiên bản khác, chọn một đường dẫn riêng và dùng cùng `PX4_DIR` khi cài và chạy. [PX4 v1.14 · chạy jMAVSim](https://docs.px4.io/v1.14/en/simulation/jmavsim.html) · [MAVSDK v3.17.5](https://github.com/mavlink/MAVSDK/releases/tag/v3.17.5)

### Build trên Windows x64

Dùng Developer PowerShell của Visual Studio 2022 có workload **Desktop development with C++**. Tải `mavsdk-windows-x64.zip` trong bản phát hành v3.17.5 vào thư mục dự án. Khi build trên cả Windows và Ubuntu, dùng thư mục build riêng cho từng hệ.

```powershell
Expand-Archive .\mavsdk-windows-x64.zip -DestinationPath .\mavsdk-v3
$sdk = (Resolve-Path .\mavsdk-v3).Path
cmake -S . -B build-win -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$sdk"
cmake --build build-win --config Release --parallel
$env:Path = "$sdk\Release\bin;$env:Path"
```

Các chương trình nằm tại `build-win\Release`. Phần giám sát có thể chạy trên Windows trong khi kịch bản bay và PX4 SITL cùng chạy trên Ubuntu hoặc WSL2. [MAVSDK v3.17.5](https://github.com/mavlink/MAVSDK/releases/tag/v3.17.5)

## 2. Chuẩn bị nguồn số đo

### ESP32-C3 thật

| Linh kiện | Kết nối ESP32-C3 |
| --- | --- |
| DHT11 | DATA → GPIO6, VCC → 3V3, GND → GND. Nếu là cảm biến rời, dùng điện trở kéo DATA lên 3V3. |
| SSD1306 I²C | SDA → GPIO4, SCL → GPIO5, VCC → 3V3, GND → GND. Firmware dò địa chỉ `0x3C` và `0x3D`. |

Trong [thư mục firmware](ESP32-C3-LTMProject-Tuan/), sao chép `include/network.example.h` thành `include/network.local.h`. Điền tên và mật khẩu Wi-Fi 2,4 GHz cùng `RECEIVER_HOST` là IPv4 LAN của **máy chạy chương trình giám sát**. Máy đó cần nhận được UDP `8080` từ mạng thử nghiệm. Tệp cấu hình cá nhân được bỏ qua khi theo dõi bằng Git.

```bash
cd ESP32-C3-LTMProject-Tuan
pio run -e esp32_c3_usb
pio device list
pio run -e esp32_c3_usb -t upload
pio device monitor -b 115200
```

Nếu có nhiều bo, chọn cổng khi nạp bằng `--upload-port COMx` trên Windows hoặc `--upload-port /dev/ttyUSB0` trên Linux. Khi bộ giám sát đã mở, OLED sẽ cập nhật nhiệt độ, độ ẩm và số ACK. Bo gửi mẫu mới mỗi 2,5 giây. ACK bật mặc định, với 600 ms mỗi lần chờ và tối đa ba lần gửi cho một mẫu.

### Giả lập cảm biến

Từ thư mục gốc, dùng Python 3 để phát số đo liên tục tới bộ giám sát trên cùng máy. Kịch bản này dùng định dạng văn bản tương thích với công cụ cũ của Khoa.

```bash
python3 tools/mock_esp32.py --scenario normal
```

Nếu dùng Wokwi trong VS Code, mở thư mục firmware, build bằng `pio run -e esp32_c3_wokwi` rồi chạy **Wokwi: Start Simulator**. Sơ đồ Wokwi dùng DHT22 và OLED SSD1306. Môi trường mô phỏng mặc định kết nối `Wokwi-GUEST` và gửi tới `host.wokwi.internal:8080` qua Private IoT Gateway. [Mạng Wi-Fi của Wokwi](https://docs.wokwi.com/guides/esp32-wifi)

## 3. Chạy giám sát và kịch bản bay

1. Trong Ubuntu hoặc WSL2, khởi động PX4. Bỏ `--headless` nếu muốn mở cửa sổ jMAVSim.

   ```bash
   bash Khoi2_UAV_PX4_Son/scripts/3_run_px4_sitl.sh --headless
   ```

2. Tại dấu nhắc `pxh>`, mở luồng MAVLink riêng đến bộ giám sát. Lệnh này dùng `127.0.0.1` khi PX4 và bộ giám sát chạy trong cùng môi trường mạng. Nếu bộ giám sát chạy trên Windows và PX4 chạy trong WSL2, thay `127.0.0.1` bằng địa chỉ Windows mà WSL2 truy cập được. Với mạng NAT của WSL2, xem địa chỉ sau chữ `via` trong kết quả `ip route show default`. Bộ giám sát cần nhận UDP `14600` trên địa chỉ đó. Lặp lệnh tại console sau mỗi lần khởi động lại PX4. [Cấu hình cổng MAVLink của PX4 v1.14.3](https://github.com/PX4/PX4-Autopilot/blob/v1.14.3/ROMFS/px4fmu_common/init.d-posix/px4-rc.mavlink) · [Kết nối mạng WSL](https://learn.microsoft.com/windows/wsl/networking)

   ```text
   mavlink start -u 14601 -o 14600 -t 127.0.0.1 -m onboard -r 1000000
   mavlink status
   ```

3. Mở bộ giám sát trong terminal thứ hai rồi bật ESP32 hoặc chạy chương trình giả lập cảm biến. Dùng lệnh sau trong Ubuntu.

   ```bash
   ./build/esp32_px4_monitor --csv monitor-v3.csv
   ```

   Trong PowerShell, mở bộ giám sát sau khi thêm DLL MAVSDK vào `Path`.

   ```powershell
   .\build-win\Release\esp32_px4_monitor.exe --csv monitor-v3.csv
   ```

4. Khi dashboard hiển thị `CONNECTED` và đã có vị trí UAV, chạy kịch bản trong một terminal Ubuntu khác. Cổng `14540` dùng luồng điều khiển sẵn của PX4. Cổng `14550` dành cho QGroundControl.

   ```bash
   ./build/uav_flight_scenario --runs 1 --csv flight-test-v3.csv
   ```

   Dùng `--runs 5` để lặp bài bay. Mỗi lượt kiểm tra điều kiện cất cánh, nạp bốn điểm ở độ cao tương đối 10 m, arm, chạy mission rồi thực hiện RTL hoặc Land. Kịch bản ghi `PASS` khi hoàn thành lộ trình và xác nhận UAV đã hạ cánh, tắt động cơ. Nếu một lượt thất bại, chương trình ghi `FAIL` và dừng các lượt sau. Dùng PX4 SITL cho quy trình này.

## Giao thức và kết quả

Firmware gửi mỗi bản tin trong một datagram UDP tối đa 256 byte.

```json
{"type":"data","session":"0123456789abcdef","seq":1,"uptime_ms":2500,"temperature_c":28.5,"humidity_pct":65.2,"sensor_valid":true,"ack_required":true}
```

- Với `sensor_valid:false`, hai trường số đo gửi `null`. ACK có dạng `ACK,0123456789abcdef,1`. Bộ giám sát trả từ chính cổng nhận UDP nếu `ack_required:true`. Khi ESP32 gửi lại một gói đã nhận trong cửa sổ, bộ giám sát trả lại ACK và CSV vẫn chỉ có một dòng cho mẫu đó. ACK xác nhận bộ nhận đã xử lý datagram.
- `session` đổi khi ESP32 khởi động lại, nên seq mới được theo dõi từ đầu. Bộ giám sát theo dõi một ESP32 và nhớ tối đa 64 phiên gần nhất để bỏ gói cũ. Mỗi phiên theo dõi 1.024 số thứ tự gần nhất, tính cả tràn uint32. Thống kê thiếu gói được cộng từ khoảng seq quan sát của từng phiên, và gói đến muộn trong cửa sổ có thể bù khoảng thiếu.
- Định dạng cũ `SEQ:1;TEMP:28.5;HUM:65.2;VALID:1` vẫn chạy trong phiên giám sát riêng. Có thể đặt lại cổng bằng `--udp-port 5005` khi dùng firmware cấu hình cũ. Nếu dùng bộ chuyển tiếp UDP của Tuấn, đặt cổng đích của bộ chuyển tiếp là cổng của monitor, mặc định `8080`.
- `monitor-v3.csv` lưu dấu thời gian UTC, seq, nhiệt độ, độ ẩm, trạng thái cảm biến, trạng thái kết nối, chế độ bay, tọa độ và độ cao tương đối. Hai cột cuối là `Sensor_Session` và `Sensor_Uptime_ms`. `UDP_LATE` đánh dấu gói đến muộn. `STATUS` ghi trạng thái `WAITING` hoặc `STALE` khi thiếu mẫu. Dùng tệp mới khi chuyển từ CSV v2. Ctrl+C kết thúc bộ giám sát và ghi hết các dòng đang chờ.
- Ngưỡng mặc định của cảm biến là 5 giây và của PX4 là 3 giây. Khi mất MAVLink, dashboard chuyển sang `RECONNECTING` và bỏ các giá trị UAV cũ. Khi cùng autopilot phát heartbeat trở lại, dashboard nhận dữ liệu của phiên mới. Dùng `--help` để xem cách đổi ngưỡng và địa chỉ MAVLink.

## Kiểm thử

Build riêng chế độ UDP bằng CMake để thử ngay khi chưa cài MAVSDK hoặc chưa có SITL.

```bash
cmake -S . -B build-udp -DMONITOR_WITH_MAVSDK=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-udp --config Release --parallel
python3 tools/kiem_thu_loopback.py ./build-udp/esp32_px4_monitor --output-dir run-udp
```

Trên Windows, dùng `python` và đường dẫn `build-udp\Release\esp32_px4_monitor.exe` nếu dùng trình tạo Visual Studio. Kiểm thử chạy qua UDP thật trên máy, kiểm tra timeout và phục hồi, seq tràn và đến muộn, JSON lỗi, ACK gửi lại, cờ tắt ACK, đổi phiên ESP32 và dữ liệu CSV. Chọn thư mục kết quả mới cho mỗi lần chạy. [Kết quả kiểm thử UDP của bản tích hợp](verification/integrated-v3-udp/ket_qua_loopback.json)

Hai môi trường firmware `esp32_c3_usb` và `esp32_c3_wokwi` đã build thành công. CMake đã tìm thấy MAVSDK 3.17.5 và mã giám sát UAV cùng kịch bản bay đã biên dịch thành mã đối tượng với bộ header này. Bước liên kết đầy đủ trên Windows cần MSVC tương thích với gói MAVSDK dựng sẵn. Kiểm thử cả hệ thống cần môi trường chạy PX4 SITL và nguồn cảm biến.

Khi có môi trường tích hợp, chạy một phiên với bo thật và một lượt SITL. Đối chiếu seq và ACK trên OLED với CSV, kiểm tra chế độ Mission và Land/RTL trên dashboard rồi xác nhận `4/4,PASS` trong CSV chuyến bay. Để thử khôi phục telemetry, dừng PX4, chờ dashboard chuyển `RECONNECTING`, khởi động PX4 và mở lại luồng `14600`. Các log trong `Khoi2_UAV_PX4_Son/logs/` và các tệp ngay dưới `verification/` ghi lại các lượt kiểm thử riêng trước khi tích hợp.
