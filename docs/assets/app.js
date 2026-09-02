(() => {
  "use strict";

  const pages = [
    {
      id: "home",
      title: "Overview",
      path: "",
      group: "Overview",
      description: "What pogopo is, what has been built, and the project at a glance.",
    },
    {
      id: "hardware",
      title: "Hardware",
      path: "hardware/",
      group: "System",
      description: "The ESP32-S3, Sharp Memory LCD, PCB, controls, storage, sensors, and power path.",
    },
    {
      id: "modularity",
      title: "Modularity",
      path: "modularity/",
      group: "System",
      description: "The work-in-progress mechanical and electrical expansion area on the right side.",
    },
    {
      id: "pogopoos",
      title: "pogopoOS",
      path: "pogopoos/",
      group: "System",
      description: "The ESP-IDF firmware stack, launcher, services, apps, games, and emulators.",
    },
    {
      id: "pogodate",
      title: "PogoDate",
      path: "pogodate/",
      group: "System",
      description: "The experimental Lua Playdate compatibility runtime, supported packages, and limitations.",
    },
    {
      id: "architecture",
      title: "Architecture",
      path: "architecture/",
      group: "System",
      description: "High-level layers from hardware and drivers to PogopoOS applications.",
    },
    {
      id: "engineering",
      title: "Engineering",
      path: "engineering/",
      group: "Project",
      description: "Measured problems, investigations, implemented solutions, and verified results.",
    },
    {
      id: "gallery",
      title: "Gallery",
      path: "gallery/",
      group: "Project",
      description: "Current launcher, quick menu, settings, animation, and future hardware photos.",
    },
    {
      id: "faq",
      title: "FAQ",
      path: "faq/",
      group: "Project",
      description: "Technical answers for reviewers, developers, and interviewers.",
    },
    {
      id: "development",
      title: "Development",
      path: "development/",
      group: "Project",
      description: "Working, experimental, and planned work plus build and SD-card setup.",
    },
    {
      id: "license",
      title: "License",
      path: "license/",
      group: "Project",
      description: "Apache-2.0 terms, third-party notices, trademarks, and project attribution.",
    },
  ];

  const body = document.body;
  const root = body.dataset.root || "./";
  const currentPage = body.dataset.page || "home";

  // The supplied frog is a transparent PNG. This SVG filter grows only its
  // alpha channel by one screen pixel and places black behind the original,
  // giving the white sprite a crisp outline in light mode without flattening
  // or changing the source image. Dark mode disables the filter entirely.
  body.insertAdjacentHTML(
    "afterbegin",
    `<svg class="pixel-filter-defs" aria-hidden="true" focusable="false">
       <defs>
         <filter id="frog-pixel-outline" x="-12%" y="-12%" width="124%" height="124%"
                 color-interpolation-filters="sRGB">
           <feMorphology in="SourceAlpha" operator="dilate" radius="1" result="expanded" />
           <feFlood flood-color="#000000" result="outline-color" />
           <feComposite in="outline-color" in2="expanded" operator="in" result="outline" />
           <feMerge><feMergeNode in="outline" /><feMergeNode in="SourceGraphic" /></feMerge>
         </filter>
       </defs>
     </svg>`,
  );

  const urlFor = (page) => `${root}${page.path}`;

  document.querySelectorAll("[data-home-link]").forEach((link) => {
    link.href = root;
  });
  document.querySelectorAll("[data-logo]").forEach((image) => {
    image.src = `${root}assets/jabaLOGO.png`;
  });
  document.querySelectorAll(".brand-label").forEach((label) => {
    label.textContent = "technical docs";
  });
  document.querySelectorAll(".sidebar-note").forEach((note) => {
    note.textContent = "Technical documentation and engineering portfolio for the pogopo handheld platform.";
  });

  const header = document.querySelector(".header-inner");
  const githubLink = header?.querySelector(".icon-link");
  const themeToggle = document.createElement("button");
  themeToggle.className = "theme-toggle";
  themeToggle.type = "button";
  themeToggle.innerHTML = `
    <svg class="sun-icon" viewBox="0 0 24 24" aria-hidden="true">
      <circle cx="12" cy="12" r="3.5" />
      <path d="M12 2v3M12 19v3M4.9 4.9 7 7M17 17l2.1 2.1M2 12h3M19 12h3M4.9 19.1 7 17M17 7l2.1-2.1" />
    </svg>
    <svg class="moon-icon" viewBox="0 0 24 24" aria-hidden="true">
      <path d="M20 15.2A8 8 0 0 1 8.8 4 8 8 0 1 0 20 15.2Z" />
    </svg>`;

  const updateThemeButton = () => {
    const isDark = document.documentElement.dataset.theme === "dark";
    const nextTheme = isDark ? "light" : "dark";
    themeToggle.setAttribute("aria-label", `Switch to ${nextTheme} mode`);
    themeToggle.title = `Switch to ${nextTheme} mode`;
  };
  updateThemeButton();
  themeToggle.addEventListener("click", () => {
    const nextTheme = document.documentElement.dataset.theme === "dark" ? "light" : "dark";
    document.documentElement.dataset.theme = nextTheme;
    try {
      localStorage.setItem("pogopo-docs-theme", nextTheme);
    } catch {
      // Theme switching still works for the current page without storage.
    }
    updateThemeButton();
  });
  if (header) header.insertBefore(themeToggle, githubLink || null);

  const groups = [...new Set(pages.map((page) => page.group))];
  const nav = document.querySelector("#site-nav");
  if (nav) {
    nav.innerHTML = groups
      .map((group) => {
        const links = pages
          .filter((page) => page.group === group)
          .map(
            (page) => `
              <li>
                <a class="nav-link${page.id === currentPage ? " active" : ""}"
                   href="${urlFor(page)}"${page.id === currentPage ? ' aria-current="page"' : ""}>
                  ${page.title}
                </a>
              </li>`,
          )
          .join("");
        return `
          <section class="nav-group" aria-labelledby="nav-${group.toLowerCase().replace(/[^a-z]+/g, "-")}">
            <h2 class="nav-heading" id="nav-${group.toLowerCase().replace(/[^a-z]+/g, "-")}">${group}</h2>
            <ul class="nav-list">${links}</ul>
          </section>`;
      })
      .join("");
  }

  const headings = [...document.querySelectorAll("#main-content h2, #main-content h3")];
  const slugify = (value) =>
    value
      .toLowerCase()
      .trim()
      .replace(/[^a-z0-9\s-]/g, "")
      .replace(/\s+/g, "-")
      .replace(/-+/g, "-");

  headings.forEach((heading, index) => {
    if (!heading.id) heading.id = slugify(heading.textContent) || `section-${index + 1}`;
  });

  const toc = document.querySelector("#page-toc");
  if (toc) {
    if (headings.length) {
      toc.innerHTML = headings
        .map(
          (heading) => `
            <li>
              <a class="toc-link level-${heading.tagName === "H3" ? "3" : "2"}"
                 href="#${heading.id}" data-toc-id="${heading.id}">${heading.textContent}</a>
            </li>`,
        )
        .join("");
    } else {
      toc.innerHTML = '<li class="toc-empty">No sections on this page.</li>';
    }
  }

  if ("IntersectionObserver" in window && headings.length) {
    const tocLinks = new Map(
      [...document.querySelectorAll("[data-toc-id]")].map((link) => [link.dataset.tocId, link]),
    );
    const visible = new Set();
    const updateToc = () => {
      const activeId = headings.find((heading) => visible.has(heading.id))?.id;
      tocLinks.forEach((link, id) => link.classList.toggle("active", id === activeId));
    };
    const observer = new IntersectionObserver(
      (entries) => {
        entries.forEach((entry) => {
          if (entry.isIntersecting) visible.add(entry.target.id);
          else visible.delete(entry.target.id);
        });
        updateToc();
      },
      { rootMargin: "-84px 0px -64% 0px" },
    );
    headings.forEach((heading) => observer.observe(heading));
  }

  const currentIndex = pages.findIndex((page) => page.id === currentPage);
  const pageNav = document.querySelector("#page-nav");
  if (pageNav && currentIndex >= 0) {
    const previous = pages[currentIndex - 1];
    const next = pages[currentIndex + 1];
    pageNav.innerHTML = `
      ${
        previous
          ? `<a class="page-nav-link" href="${urlFor(previous)}">
               <span class="page-nav-kicker">Previous</span>
               <span class="page-nav-title">← ${previous.title}</span>
             </a>`
          : "<span></span>"
      }
      ${
        next
          ? `<a class="page-nav-link next" href="${urlFor(next)}">
               <span class="page-nav-kicker">Next</span>
               <span class="page-nav-title">${next.title} →</span>
             </a>`
          : ""
      }`;
  }

  const menuToggle = document.querySelector("#menu-toggle");
  const navScrim = document.createElement("div");
  navScrim.className = "nav-scrim";
  navScrim.setAttribute("aria-hidden", "true");
  document.body.append(navScrim);

  const closeNav = () => {
    body.classList.remove("nav-open");
    menuToggle?.setAttribute("aria-expanded", "false");
  };
  menuToggle?.addEventListener("click", () => {
    const open = body.classList.toggle("nav-open");
    menuToggle.setAttribute("aria-expanded", String(open));
  });
  navScrim.addEventListener("click", closeNav);
  nav?.addEventListener("click", (event) => {
    if (event.target.closest("a")) closeNav();
  });

  const searchModal = document.createElement("div");
  searchModal.className = "search-modal";
  searchModal.setAttribute("role", "dialog");
  searchModal.setAttribute("aria-modal", "true");
  searchModal.setAttribute("aria-label", "Search documentation");
  searchModal.innerHTML = `
    <div class="search-panel">
      <div class="search-input-wrap">
        <input class="search-input" type="search" placeholder="Search pogopo docs…" aria-label="Search pogopo docs">
      </div>
      <ul class="search-results" aria-live="polite"></ul>
    </div>`;
  document.body.append(searchModal);

  const searchInput = searchModal.querySelector(".search-input");
  const searchResults = searchModal.querySelector(".search-results");

  const renderSearch = (query = "") => {
    const clean = query.toLowerCase().trim();
    const matches = clean
      ? pages.filter((page) => `${page.title} ${page.description} ${page.group}`.toLowerCase().includes(clean))
      : pages;
    searchResults.innerHTML = matches.length
      ? matches
          .map(
            (page) => `
              <li>
                <a class="search-result-link" href="${urlFor(page)}">
                  <span class="search-result-title">${page.title}</span>
                  <span class="search-result-copy">${page.description}</span>
                </a>
              </li>`,
          )
          .join("")
      : '<li class="search-empty">No matching page yet.</li>';
  };

  const openSearch = () => {
    renderSearch();
    searchModal.classList.add("open");
    requestAnimationFrame(() => searchInput.focus());
  };
  const closeSearch = () => searchModal.classList.remove("open");

  document.querySelector("#search-trigger")?.addEventListener("click", openSearch);
  searchInput.addEventListener("input", () => renderSearch(searchInput.value));
  searchModal.addEventListener("mousedown", (event) => {
    if (event.target === searchModal) closeSearch();
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "/" && !/input|textarea/i.test(document.activeElement.tagName)) {
      event.preventDefault();
      openSearch();
    }
    if (event.key === "Escape") {
      closeSearch();
      closeNav();
    }
  });

  document.querySelectorAll(".code-block").forEach((block) => {
    const code = block.querySelector("code");
    if (!code) return;
    const button = document.createElement("button");
    button.className = "copy-code";
    button.type = "button";
    button.textContent = "Copy";
    button.addEventListener("click", async () => {
      try {
        await navigator.clipboard.writeText(code.textContent);
        button.textContent = "Copied";
        setTimeout(() => (button.textContent = "Copy"), 1300);
      } catch {
        button.textContent = "Select text";
      }
    });
    block.append(button);
  });

  document.querySelectorAll("[data-year]").forEach((node) => {
    node.textContent = new Date().getFullYear();
  });
})();
