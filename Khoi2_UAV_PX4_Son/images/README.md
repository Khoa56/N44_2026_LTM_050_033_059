# Danh mục ảnh minh họa (Khối 2 — UAV Mô phỏng PX4 & MAVSDK C++)

| Thư mục / Tên file | Nội dung ảnh | Trạng thái |
| :--- | :--- | :--- |
| `01_mo_phong_px4_jmavsim/01_khoi_chay_script_tich_hop_px4_sitl.png` | Khởi chạy `uav.bat` / `5_test_tich_hop.sh`: bật PX4 SITL, mở luồng MAVLink UDP `14600` cho Khoa và chạy `uav_flight_scenario` trên `udpin://127.0.0.1:14540` | Đạt (Mới) |
| `01_mo_phong_px4_jmavsim/02_px4_khoi_tao_home_set_san_sang.png` | Log khởi tạo PX4 SITL hoàn tất (`home set`, `notify positive`, `Ready for takeoff!`) khi mở console `pxh>` | Đạt (Tái sử dụng) |
| `01_mo_phong_px4_jmavsim/03_jmavsim_3d_uav_tren_mat_dat.jpg` | Cửa sổ 3D jMAVSim hiển thị UAV ở vị trí ban đầu trên mặt đất | Đạt (Tái sử dụng) |
| `01_mo_phong_px4_jmavsim/04_jmavsim_3d_uav_dang_bay.jpg` | Cửa sổ 3D jMAVSim hiển thị UAV đang bay trên không | Đạt (Tái sử dụng) |
| `02_kich_ban_bay_mavsdk/01_buoc1_preflight_health_check.jpg` | `[Bước 1/5]` Pre-flight Health Checks (`GlobalPos`, `HomePos`, `Armable`) $\rightarrow$ `[OK]` và trạng thái `TRƯỚC KHI CẤT CÁNH` (giờ UTC `2026-11-18T07:38:28Z`) | Đạt (Mới) |
| `02_kich_ban_bay_mavsdk/02_buoc2_3_nap_waypoints_va_cat_canh.jpg` | `[Bước 2/5]` & `[Bước 3/5]` Nạp 4 waypoints độ cao `10.00 m`, Arm động cơ, chuyển `FlightMode -> Mission` và `Takeoff detected` | Đạt (Nên bổ sung thêm 1 ảnh đang bay `WAYPOINT 1/4..3/4` ở độ cao ~`10 m` nếu muốn minh họa chi tiết) |
| `02_kich_ban_bay_mavsdk/03_buoc4_5_rtl_va_ha_canh.jpg` | `[Bước 4/5]` & `[Bước 5/5]` Ra lệnh `RTL`, snapshot đang bay `IN_AIR \| ARMED` ở độ cao `9.98 m` và quá trình hạ cánh về Home | Đạt (Mới) |
| `02_kich_ban_bay_mavsdk/04_ket_thuc_luot_bay_1_1.png` | Trạng thái `ĐÃ HẠ CÁNH & HOÀN TẤT` (`ON_GROUND \| DISARMED`), thông báo hoàn tất `1/1 (100.0%)` và ghi vào `test-son-bay.csv` | Đạt (Mới) |
| `03_kiem_thu_5_luot_va_csv/01_tong_ket_chay_thu_5_luot.png` | Tổng kết chạy thử 5 lượt liên tiếp `5 / 5 (100.0%)`, lưu tại `flight-test-v3.csv` | Đạt (Mới) |
| `03_kiem_thu_5_luot_va_csv/02_file_flight_test_v3_csv.png` | Nội dung file `flight-test-v3.csv` (1 header + 5 lượt `PASS` độ cao `10.01 m`, timestamp UTC `2026-11-18T08:15:17Z`–`08:22:32Z`) | Đạt (Mới) |
| `04_xu_ly_su_co_mat_ket_noi/01_bat_loi_mat_ket_noi_px4_khi_bay.png` | Bắt lỗi `heartbeats timed out` và `UAV MẤT KẾT NỐI (DISCONNECTED)!` khi ngắt PX4 giữa chuyến bay, tổng kết `0 / 1 (0.0%)` | Đạt (Mới) |
| `05_tich_hop_song_song_khoa/` | Ảnh chạy song song `uav.bat` (`14540` + luồng `14600`) với `monitor.bat` + ESP32 thật, và ảnh dashboard Khoa thử `RECONNECTING` $\rightarrow$ `CONNECTED` | **Chờ chụp cùng Khoa** |
