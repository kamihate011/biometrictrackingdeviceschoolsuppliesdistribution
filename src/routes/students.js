const express = require("express");
const Student = require("../models/Student");
const ADMIN_PASSWORD = process.env.ADMIN_PASSWORD || "";

const MALE_SUPPLIES = [
  "Polo Uniform",
  "Slacks Pants",
  "PE Pants",
  "PE Shirt",
  "Rubber Shoes",
  "Black Shoes"
];

const FEMALE_SUPPLIES = [
  "Blouse",
  "Skirt",
  "Neck Tie",
  "PE Shirt",
  "PE Pants",
  "Doll Shoes",
  "Rubber Shoes"
];

function normalizeGender(value) {
  return String(value || "")
    .trim()
    .toUpperCase();
}

function requiresStrand(gradeLevel) {
  const normalized = String(gradeLevel || "").trim();
  return normalized === "11" || normalized === "12" || normalized === "Grade 11" || normalized === "Grade 12";
}

function validateSuppliesByGender(gender, supplies) {
  const allowed = gender === "MALE" ? MALE_SUPPLIES : FEMALE_SUPPLIES;
  return supplies.every((item) => allowed.includes(item));
}

function sanitizeAndValidateProfile(input, { requireFingerprintId = true } = {}) {
  const {
    fingerprintId,
    gender,
    lrn,
    fullName,
    gradeLevel,
    strand,
    section,
    assignedSupplies
  } = input;

  if (requireFingerprintId && (!fingerprintId || Number.isNaN(Number(fingerprintId)))) {
    return { error: "fingerprintId must be numeric." };
  }

  const normalizedGender = normalizeGender(gender);
  if (!["MALE", "FEMALE"].includes(normalizedGender)) {
    return { error: "Gender must be MALE or FEMALE." };
  }

  const supplies = Array.isArray(assignedSupplies)
    ? assignedSupplies.map((s) => String(s).trim()).filter(Boolean)
    : [];

  if (!supplies.length) {
    return { error: "Select at least one school supply." };
  }

  if (!validateSuppliesByGender(normalizedGender, supplies)) {
    return { error: "Selected supplies do not match the chosen gender set." };
  }

  const normalizedGrade = String(gradeLevel || "").trim();
  const normalizedStrand = String(strand || "").trim();
  const normalizedLrn = String(lrn || "").trim();
  const normalizedName = String(fullName || "").trim();
  const normalizedSection = String(section || "").trim();

  if (!normalizedGrade || !normalizedLrn || !normalizedName || !normalizedSection) {
    return { error: "LRN, full name, grade level, and section are required." };
  }

  if (requiresStrand(normalizedGrade) && !normalizedStrand) {
    return { error: "Strand is required for Grade 11 and 12." };
  }

  const payload = {
    gender: normalizedGender,
    lrn: normalizedLrn,
    fullName: normalizedName,
    gradeLevel: normalizedGrade,
    strand: requiresStrand(normalizedGrade) ? normalizedStrand : "",
    section: normalizedSection,
    assignedSupplies: supplies
  };

  if (requireFingerprintId) {
    payload.fingerprintId = Number(fingerprintId);
  }

  return { payload };
}

function createStudentRouter(io) {
  const router = express.Router();

  function requireAdmin(req, res, next) {
    if (!ADMIN_PASSWORD) {
      return res.status(503).json({ message: "Admin password is not configured on server." });
    }
    const provided = String(req.headers["x-admin-password"] || "");
    if (provided !== ADMIN_PASSWORD) {
      return res.status(401).json({ message: "Unauthorized admin access." });
    }
    return next();
  }

  router.post("/admin/verify", (req, res) => {
    if (!ADMIN_PASSWORD) {
      return res.status(503).json({ message: "Admin password is not configured on server." });
    }
    const provided = String(req.body?.password || "");
    if (!provided || provided !== ADMIN_PASSWORD) {
      return res.status(401).json({ message: "Invalid admin password." });
    }
    return res.json({ ok: true });
  });

  router.get("/", async (req, res, next) => {
    try {
      const students = await Student.find().sort({ fullName: 1 }).lean();
      res.json(students);
    } catch (error) {
      next(error);
    }
  });

  router.post("/", requireAdmin, async (req, res, next) => {
    try {
      const { payload, error } = sanitizeAndValidateProfile(req.body, { requireFingerprintId: true });
      if (error) {
        return res.status(400).json({ message: error });
      }

      const created = await Student.create({
        ...payload,
        distributionStatus: "PENDING",
        distributedAt: null,
        lastScannedAt: null
      });
      io.emit("student:created", created);
      res.status(201).json(created);
    } catch (error) {
      next(error);
    }
  });

  router.put("/:id/distribution", async (req, res, next) => {
    try {
      const { status } = req.body;
      if (!["PENDING", "DISTRIBUTED"].includes(status)) {
        return res.status(400).json({ message: "Invalid distribution status." });
      }

      const update = {
        distributionStatus: status,
        distributedAt: status === "DISTRIBUTED" ? new Date() : null
      };

      const student = await Student.findByIdAndUpdate(req.params.id, update, {
        new: true
      });

      if (!student) {
        return res.status(404).json({ message: "Student not found." });
      }

      io.emit("student:updated", student);
      return res.json(student);
    } catch (error) {
      next(error);
    }
  });

  router.post("/scan", async (req, res, next) => {
    try {
      const { fingerprintId } = req.body;

      if (!fingerprintId || Number.isNaN(Number(fingerprintId))) {
        return res.status(400).json({ message: "fingerprintId must be numeric." });
      }

      const student = await Student.findOneAndUpdate(
        { fingerprintId: Number(fingerprintId) },
        { lastScannedAt: new Date() },
        { new: true }
      );

      if (!student) {
        const payload = {
          fingerprintId: Number(fingerprintId),
          scannedAt: new Date().toISOString()
        };
        io.emit("scan:unknown", payload);
        return res.json({ matched: false, ...payload });
      }

      io.emit("scan:matched", student);
      return res.json(student);
    } catch (error) {
      next(error);
    }
  });

  router.post("/profile-confirm", async (req, res, next) => {
    try {
      const { payload, error } = sanitizeAndValidateProfile(req.body, { requireFingerprintId: true });
      if (error) {
        return res.status(400).json({ message: error });
      }

      const update = {
        ...payload,
        distributionStatus: "DISTRIBUTED",
        distributedAt: new Date(),
        lastScannedAt: new Date()
      };

      const student = await Student.findOneAndUpdate(
        { fingerprintId: payload.fingerprintId },
        update,
        { new: true, upsert: true, setDefaultsOnInsert: true, runValidators: true }
      );

      io.emit("student:updated", student);
      io.emit("scan:matched", student);
      return res.json(student);
    } catch (error) {
      next(error);
    }
  });

  router.put("/:id", requireAdmin, async (req, res, next) => {
    try {
      const { payload, error } = sanitizeAndValidateProfile(req.body, { requireFingerprintId: true });
      if (error) {
        return res.status(400).json({ message: error });
      }

      const student = await Student.findByIdAndUpdate(
        req.params.id,
        {
          ...payload
        },
        { new: true, runValidators: true }
      );

      if (!student) {
        return res.status(404).json({ message: "Student not found." });
      }

      io.emit("student:updated", student);
      return res.json(student);
    } catch (error) {
      next(error);
    }
  });

  router.delete("/:id", requireAdmin, async (req, res, next) => {
    try {
      const student = await Student.findByIdAndDelete(req.params.id);
      if (!student) {
        return res.status(404).json({ message: "Student not found." });
      }

      io.emit("student:deleted", { _id: student._id.toString() });
      return res.json({ ok: true });
    } catch (error) {
      next(error);
    }
  });

  router.get("/supply-options", (req, res) => {
    res.json({
      male: MALE_SUPPLIES,
      female: FEMALE_SUPPLIES
    });
  });

  return router;
}

module.exports = { createStudentRouter };
