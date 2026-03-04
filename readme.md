# Biometric Distribution System (ESP32 + MongoDB)

Database-driven web application for student supply distribution with fingerprint verification.

## Features
- Stores student records in MongoDB:
  - `LRN`
  - `fullName`
  - `gradeLevel`
  - `section`
  - `strand`
  - `assignedSupplies`
  - `distributionStatus`
- ESP32 posts matched `fingerprintId` after scan.
- Web app auto-displays matched student profile.
- Operator confirms distribution using one button.
- Distribution status updates in real time via Socket.IO.
- Mobile/tablet-friendly interface.

## Project Structure
- `src/server.js` - Express API + Socket.IO server
- `src/models/Student.js` - MongoDB student schema
- `src/routes/students.js` - CRUD + scan + distribution endpoints
- `public/` - Front-end UI
- `firmware/esp32_biometric_client.ino` - ESP32 code for R307S + OLED + RTC

## Setup
1. Install dependencies:
   ```bash
   npm install
   ```
2. Copy env file:
   ```bash
   copy .env.example .env
   ```
3. Update `.env` with your MongoDB URI.
   - Add admin password for Admin Mode:
   ```env
   ADMIN_PASSWORD=your_secure_password
   ```
4. (Optional) Seed sample records:
   ```bash
   npm run seed
   ```
5. Run server:
   ```bash
   npm run dev
   ```
6. Open from phone/tablet in same network:
   - `http://<PC_IP>:3000`

## API Endpoints
- `GET /api/health`
- `GET /api/students`
- `POST /api/students`
- `POST /api/students/admin/verify` body:
  ```json
  { "password": "your_admin_password" }
  ```
- `POST /api/students/scan` body:
  ```json
  { "fingerprintId": 1 }
  ```
- `PUT /api/students/:id/distribution` body:
  ```json
  { "status": "DISTRIBUTED" }
  ```
- `PUT /api/students/:id` (admin password required via `x-admin-password` header)
- `DELETE /api/students/:id` (admin password required via `x-admin-password` header)

## Sample Student Payload
```json
{
  "lrn": "100000000003",
  "fullName": "Jane Garcia",
  "gradeLevel": "Grade 11",
  "section": "STEM-B",
  "strand": "STEM",
  "assignedSupplies": ["Notebook", "Ballpen", "Pad Paper"],
  "distributionStatus": "PENDING",
  "fingerprintId": 3
}
```

## Hardware Wiring (ESP32 30-pin board)
Use common ground for all devices.

### R307S Fingerprint Module
- `VCC` -> `5V`
- `GND` -> `GND`
- `TX` -> `GPIO16` (ESP32 RX2)
- `RX` -> `GPIO17` (ESP32 TX2)

### 0.96 OLED (SSD1306 I2C)
- `VCC` -> `3.3V`
- `GND` -> `GND`
- `SCL` -> `GPIO22`
- `SDA` -> `GPIO21`

### DS1307 RTC (I2C shared with OLED)
- `VCC` -> `5V`
- `GND` -> `GND`
- `SCL` -> `GPIO22`
- `SDA` -> `GPIO21`

## ESP32 Notes
- Open `firmware/esp32_biometric_client.ino`.
- Set:
  - `WIFI_SSID`
  - `WIFI_PASSWORD`
  - `SERVER_URL` (example: `http://192.168.1.10:3000/api/students/scan`)
- Ensure the `fingerprintId` enrolled in R307S matches `fingerprintId` in MongoDB.

## Firebase Variant (R307S -> ESP32 -> Firebase Realtime DB)
If you prefer a serverless data pipe to a Netlify-hosted dashboard, use `firmware/esp32_r307s_firebase_client.ino`.

### What it does
- Connects ESP32 to Wi-Fi.
- Verifies fingerprint against templates stored in the R307S.
- Pushes matched IDs to Firebase Realtime Database under `/logs` using server timestamp.

### Firebase setup
1. Create a Firebase project and Realtime Database.
2. Enable Authentication (Email/Password) and create a dedicated device account.
3. In `esp32_r307s_firebase_client.ino`, set:
   - `API_KEY`
   - `DATABASE_URL`
   - `USER_EMAIL`
   - `USER_PASSWORD`
4. Keep credentials private (do not commit production secrets).

### Realtime Database shape
```json
{
  "users": {
    "1": { "name": "John Doe" }
  },
  "logs": {
    "-Nxyz...": {
      "user_id": 1,
      "timestamp": 1735689600000
    }
  }
}
```

### Notes
- **Power safety:** Do not power the R307S from an ESP32 GPIO pin. Use a stable 5V rail and common GND. If your ESP32 gets hot, disconnect power and re-check wiring immediately.
- **Buzzer wiring:** This sketch defaults buzzer output to disabled (`BUZZER_ENABLED=false`). Enable only after confirming buzzer module type and wiring; use a transistor driver for higher-current buzzers.
- The R307S usually sends matched template IDs, not full fingerprint images.
- For multi-device template sync, exporting/importing template data is required and is more advanced than scan logging.
- The Firebase sketch includes OLED status output (SSD1306 over I2C on GPIO21/22) and buzzer feedback (GPIO25 by default). Adjust pins if your board wiring differs.
- Buzzer output uses simple digital pulse mode for broad compatibility (active buzzer modules). If your buzzer is active-low, set `BUZZER_ACTIVE_HIGH` to `false` in the sketch.
- Fingerprint initialization now auto-tries common baud rates and both RX/TX wiring directions to recover from module defaults and swapped wires.


### Netlify deploy note
If Netlify shows messages like "All files already uploaded" and "No redirect/header/function rules processed," that simply means no changed artifacts or Netlify-specific rule files were included in that deploy.
- The R307S usually sends matched template IDs, not full fingerprint images.
- For multi-device template sync, exporting/importing template data is required and is more advanced than scan logging.
- The Firebase sketch includes OLED status output (SSD1306 over I2C on GPIO21/22) and buzzer feedback (GPIO25 by default). Adjust pins if your board wiring differs.

## Deployment/Usage Flow
1. Student places finger on R307S.
2. ESP32 sends `fingerprintId` to `/api/students/scan`.
3. Server finds student and emits `scan:matched`.
4. Tablet/phone UI immediately shows student record.
5. Operator taps `Confirm Distribution`.
6. Server updates MongoDB status and emits realtime update.
