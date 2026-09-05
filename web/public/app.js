const form = document.getElementById("member-form");
const messageEl = document.getElementById("form-message");
const tbody = document.querySelector("#member-table tbody");

function showMessage(text, kind) {
  messageEl.textContent = text;
  messageEl.className = "message" + (kind ? " " + kind : "");
}

function escapeHtml(str) {
  return String(str ?? "").replace(/[&<>"']/g, (c) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  }[c]));
}

async function loadMembers() {
  const res = await fetch("/api/members");
  const members = await res.json();
  tbody.innerHTML = members.map((m) => `
    <tr>
      <td>${escapeHtml(m.id)}</td>
      <td>${escapeHtml(m.username)}</td>
      <td>${escapeHtml(m.name)}</td>
      <td>${escapeHtml(m.email)}</td>
      <td>${escapeHtml(m.phone)}</td>
      <td>${escapeHtml(m.address)}</td>
      <td>${escapeHtml(m.birth_date)}</td>
      <td>${escapeHtml((m.created_at || "").replace("T", " ").slice(0, 19))}</td>
      <td><button class="delete-btn" data-id="${m.id}">삭제</button></td>
    </tr>
  `).join("");
}

form.addEventListener("submit", async (e) => {
  e.preventDefault();
  const data = Object.fromEntries(new FormData(form).entries());
  showMessage("등록 중...");
  try {
    const res = await fetch("/api/members", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(data),
    });
    const body = await res.json();
    if (!res.ok) throw new Error(body.error || "등록에 실패했습니다.");
    showMessage(`등록되었습니다 (ID ${body.id}).`, "success");
    form.reset();
    await loadMembers();
  } catch (err) {
    showMessage(err.message, "error");
  }
});

tbody.addEventListener("click", async (e) => {
  const btn = e.target.closest(".delete-btn");
  if (!btn) return;
  if (!confirm("이 회원을 삭제할까요?")) return;
  await fetch(`/api/members/${btn.dataset.id}`, { method: "DELETE" });
  await loadMembers();
});

loadMembers();
