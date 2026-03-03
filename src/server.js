require("dotenv").config();
const path = require("path");
const express = require("express");
const cors = require("cors");
const http = require("http");
const { Server } = require("socket.io");
const { connectDatabase } = require("./config/db");
const { createStudentRouter } = require("./routes/students");

const app = express();
const server = http.createServer(app);
const io = new Server(server, {
  cors: {
    origin: "*"
  }
});

const PORT = Number(process.env.PORT || 3000);
const MONGODB_URI = process.env.MONGODB_URI || "";

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, "..", "public")));

app.get("/api/health", (req, res) => {
  res.json({ ok: true, timestamp: new Date().toISOString() });
});

app.use("/api/students", createStudentRouter(io));

app.use((req, res) => {
  res.status(404).json({ message: "Route not found." });
});

app.use((error, req, res, next) => {
  if (error.code === 11000) {
    return res.status(409).json({ message: "Duplicate value detected.", detail: error.keyValue });
  }

  return res.status(500).json({ message: "Internal server error.", detail: error.message });
});

async function start() {
  if (!MONGODB_URI) {
    throw new Error("MONGODB_URI is missing. Add it to .env");
  }

  await connectDatabase(MONGODB_URI);
  server.listen(PORT, () => {
    console.log(`Server running on http://localhost:${PORT}`);
  });
}

start().catch((error) => {
  console.error("Failed to start server:", error.message);
  process.exit(1);
});
