(() => {
  "use strict";

  const storageKey = "pogopo-docs-theme";
  let savedTheme = null;
  try {
    savedTheme = localStorage.getItem(storageKey);
  } catch {
    // The site still follows the system theme when storage is unavailable.
  }

  const systemTheme = window.matchMedia?.("(prefers-color-scheme: dark)").matches
    ? "dark"
    : "light";
  const theme = savedTheme === "light" || savedTheme === "dark" ? savedTheme : systemTheme;
  document.documentElement.dataset.theme = theme;
})();
