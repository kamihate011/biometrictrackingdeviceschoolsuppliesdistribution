require("dotenv").config();
const { connectDatabase } = require("./config/db");
const Student = require("./models/Student");

const demoStudents = [
  {
    gender: "FEMALE",
    lrn: "100000000001",
    fullName: "Maria Santos",
    gradeLevel: "Grade 11",
    section: "STEM-A",
    strand: "STEM",
    assignedSupplies: ["Notebook", "Ballpen", "Ruler"],
    distributionStatus: "PENDING",
    fingerprintId: 1
  },
  {
    gender: "MALE",
    lrn: "100000000002",
    fullName: "John Dela Cruz",
    gradeLevel: "Grade 12",
    section: "ABM-B",
    strand: "ABM",
    assignedSupplies: ["Notebook", "Ballpen", "Calculator"],
    distributionStatus: "PENDING",
    fingerprintId: 2
  }
];

async function run() {
  const uri = process.env.MONGODB_URI;
  if (!uri) {
    throw new Error("MONGODB_URI is missing. Add it to .env");
  }

  await connectDatabase(uri);
  await Student.deleteMany({});
  await Student.insertMany(demoStudents);
  console.log("Seed data inserted.");
  process.exit(0);
}

run().catch((error) => {
  console.error("Seed failed:", error.message);
  process.exit(1);
});
