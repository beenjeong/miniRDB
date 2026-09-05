const path = require("path");
const express = require("express");
const bcrypt = require("bcryptjs");
const db = require("./db");

const app = express();
app.use(express.json());
app.use(express.static(path.join(__dirname, "public")));

const REQUIRED_FIELDS = ["username", "password", "name"];
const MAX_LEN = 200; // keep well under miniRDB's 4KB-per-row page limit

function validate(body) {
  for (const field of REQUIRED_FIELDS) {
    if (!body[field] || !String(body[field]).trim()) return `${field}은(는) 필수 항목입니다.`;
  }
  for (const [key, value] of Object.entries(body)) {
    if (value && String(value).length > MAX_LEN) return `${key} 값이 너무 깁니다 (최대 ${MAX_LEN}자).`;
  }
  return null;
}

app.get("/api/members", async (req, res) => {
  try {
    const members = await db.listMembers();
    res.json(members);
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

app.post("/api/members", async (req, res) => {
  try {
    const body = req.body || {};
    const err = validate(body);
    if (err) return res.status(400).json({ error: err });

    if (await db.findByUsername(body.username)) {
      return res.status(409).json({ error: "이미 사용 중인 아이디입니다." });
    }

    const passwordHash = await bcrypt.hash(String(body.password), 10);
    const id = await db.insertMember({
      username: body.username,
      passwordHash,
      name: body.name,
      email: body.email || "",
      phone: body.phone || "",
      address: body.address || "",
      birth_date: body.birth_date || "",
    });
    res.status(201).json({ id });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

app.delete("/api/members/:id", async (req, res) => {
  try {
    await db.deleteMember(req.params.id);
    res.status(204).end();
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
  console.log(`minirdb-web listening on http://localhost:${PORT}`);
});
