const mongoose = require("mongoose");

const studentSchema = new mongoose.Schema(
  {
    gender: { type: String, enum: ["MALE", "FEMALE"], required: true },
    lrn: { type: String, required: true, unique: true, trim: true },
    fullName: { type: String, required: true, trim: true },
    gradeLevel: { type: String, required: true, trim: true },
    section: { type: String, required: true, trim: true },
    strand: { type: String, default: "", trim: true },
    assignedSupplies: [{ type: String, trim: true }],
    distributionStatus: {
      type: String,
      enum: ["PENDING", "DISTRIBUTED"],
      default: "PENDING"
    },
    fingerprintId: { type: Number, required: true, unique: true, min: 1 },
    distributedAt: { type: Date, default: null },
    lastScannedAt: { type: Date, default: null }
  },
  { timestamps: true }
);

module.exports = mongoose.model("Student", studentSchema);
