(() => {
  "use strict";

  const pages = window.AETHER_DOCS || {};
  const nav = document.getElementById("doc-nav");
  const article = document.getElementById("doc-content");
  const search = document.getElementById("doc-search");
  const title = document.getElementById("page-title");

  function escapeHtml(value) {
    return String(value).replace(/[&<>"']/g, ch => ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[ch]));
  }

  function slug(value) {
    return value.toLowerCase().replace(/[^\p{L}\p{N}\s-]/gu, "").trim().replace(/[\s]+/g, "-");
  }

  function resolvePage(source, target) {
    const clean = target.split(/[?#]/, 1)[0];
    if (!clean || !clean.toLowerCase().endsWith(".md")) return null;
    const parts = source.split("/");
    parts.pop();
    for (const part of clean.replaceAll("\\", "/").split("/")) {
      if (!part || part === ".") continue;
      if (part === "..") parts.pop();
      else parts.push(part);
    }
    return parts.join("/");
  }

  function inline(text, source) {
    let value = escapeHtml(text);
    value = value.replace(/`([^`]+)`/g, "<code>$1</code>");
    value = value.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
    value = value.replace(/\*([^*]+)\*/g, "<em>$1</em>");
    value = value.replace(/!\[([^\]]*)\]\(([^)]+)\)/g, (_all, alt, url) => `<img alt="${alt}" src="${escapeHtml(url)}">`);
    value = value.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (_all, label, url) => {
      const target = url.trim();
      if (/^(https?:|mailto:)/i.test(target)) return `<a href="${escapeHtml(target)}" rel="noreferrer">${label}</a>`;
      const local = resolvePage(source, target);
      if (local && Object.hasOwn(pages, local)) {
        const anchor = target.includes("#") ? `#${target.split("#").slice(1).join("#")}` : "";
        return `<a href="#/${encodeURIComponent(local)}${anchor}">${label}</a>`;
      }
      if (target.startsWith("#")) return `<a href="${escapeHtml(target)}">${label}</a>`;
      return `<a href="${escapeHtml(target)}">${label}</a>`;
    });
    return value;
  }

  function renderMarkdown(markdown, source) {
    const lines = markdown.replace(/\r/g, "").split("\n");
    const html = [];
    let paragraph = [];
    let list = "";
    let code = null;
    let codeLines = [];
    let tableRows = [];
    const flushParagraph = () => { if (paragraph.length) html.push(`<p>${inline(paragraph.join(" "), source)}</p>`); paragraph = []; };
    const flushList = () => { if (list) { html.push(`</${list}>`); list = ""; } };
    const flushTable = () => {
      if (!tableRows.length) return;
      const rows = tableRows.filter(row => !/^\|?\s*:?-{3,}/.test(row));
      if (rows.length) {
        const cells = row => row.replace(/^\s*\||\|\s*$/g, "").split("|").map(cell => cell.trim());
        const head = cells(rows[0]);
        html.push("<table><thead><tr>" + head.map(cell => `<th>${inline(cell, source)}</th>`).join("") + "</tr></thead><tbody>");
        for (const row of rows.slice(1)) html.push("<tr>" + cells(row).map(cell => `<td>${inline(cell, source)}</td>`).join("") + "</tr>");
        html.push("</tbody></table>");
      }
      tableRows = [];
    };
    for (const line of lines) {
      const fence = line.match(/^\s*```(.*)$/);
      if (fence) {
        flushParagraph(); flushList(); flushTable();
        if (code === null) { code = fence[1].trim(); codeLines = []; }
        else { html.push(`<pre><code${code ? ` class="language-${escapeHtml(code)}"` : ""}>${escapeHtml(codeLines.join("\n"))}</code></pre>`); code = null; }
        continue;
      }
      if (code !== null) { codeLines.push(line); continue; }
      if (/^\s*\|.*\|\s*$/.test(line)) { flushParagraph(); flushList(); tableRows.push(line); continue; }
      flushTable();
      const heading = line.match(/^\s{0,3}(#{1,6})\s+(.+?)\s*#*\s*$/);
      if (heading) {
        flushParagraph(); flushList();
        const level = heading[1].length;
        const text = heading[2];
        html.push(`<h${level} id="${escapeHtml(slug(text))}">${inline(text, source)}</h${level}>`);
      } else if (/^\s*[-*+]\s+/.test(line) || /^\s*\d+[.)]\s+/.test(line)) {
        flushParagraph();
        const ordered = /^\s*\d+[.)]\s+/.test(line);
        const type = ordered ? "ol" : "ul";
        if (list !== type) { flushList(); list = type; html.push(`<${list}>`); }
        html.push(`<li>${inline(line.replace(/^\s*(?:[-*+]\s+|\d+[.)]\s+)/, ""), source)}</li>`);
      } else if (/^\s*>\s?/.test(line)) {
        flushParagraph(); flushList();
        html.push(`<blockquote>${inline(line.replace(/^\s*>\s?/, ""), source)}</blockquote>`);
      } else if (!line.trim()) {
        flushParagraph(); flushList();
      } else {
        flushList(); paragraph.push(line.trim());
      }
    }
    if (code !== null) html.push(`<pre><code>${escapeHtml(codeLines.join("\n"))}</code></pre>`);
    flushParagraph(); flushList(); flushTable();
    return html.join("\n");
  }

  function pageTitle(page) {
    const heading = page.content.match(/^#\s+(.+)$/m);
    return heading ? heading[1].replace(/[`*_]/g, "") : page.path;
  }

  function currentPath() {
    const route = decodeURIComponent(location.hash.replace(/^#\/?/, ""));
    const path = route.split("#", 1)[0];
    return Object.hasOwn(pages, path) ? path : (Object.hasOwn(pages, "manual/README.md") ? "manual/README.md" : Object.keys(pages)[0]);
  }

  function drawNav(filter = "") {
    const needle = filter.toLowerCase();
    nav.replaceChildren();
    for (const [path, page] of Object.entries(pages)) {
      const label = pageTitle(page);
      if (needle && !(label + " " + path + " " + page.content).toLowerCase().includes(needle)) continue;
      const link = document.createElement("a");
      link.href = `#/${encodeURIComponent(path)}`;
      link.textContent = label;
      link.title = path;
      link.dataset.path = path;
      nav.append(link);
    }
    const active = currentPath();
    const selected = nav.querySelector(`[data-path="${CSS.escape(active)}"]`);
    if (selected) selected.classList.add("active");
  }

  function showPage() {
    const path = currentPath();
    const page = pages[path];
    if (!page) return;
    const label = pageTitle(page);
    title.textContent = label;
    document.title = `${label} · Aether Documentation`;
    article.innerHTML = renderMarkdown(page.content, path);
    drawNav(search.value);
    if (location.hash.includes("#", 2)) {
      const id = decodeURIComponent(location.hash.slice(location.hash.indexOf("#", 2) + 1));
      requestAnimationFrame(() => document.getElementById(id)?.scrollIntoView());
    } else article.scrollTop = 0;
  }

  search.addEventListener("input", () => drawNav(search.value));
  window.addEventListener("hashchange", showPage);
  drawNav();
  showPage();
})();
