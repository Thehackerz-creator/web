// ============================================================
// ATLAS site — shared interactions (nav, reveal-on-scroll, docs sidebar)
// ============================================================

document.addEventListener('DOMContentLoaded', () => {

  // Reveal-on-scroll
  const revealEls = document.querySelectorAll('.reveal');
  if ('IntersectionObserver' in window && revealEls.length){
    const io = new IntersectionObserver((entries) => {
      entries.forEach(e => { if (e.isIntersecting){ e.target.classList.add('in'); io.unobserve(e.target); } });
    }, { threshold: 0.12 });
    revealEls.forEach(el => io.observe(el));
  } else {
    revealEls.forEach(el => el.classList.add('in'));
  }

  // Docs sidebar active-link tracking
  const steps = document.querySelectorAll('.step[id]');
  const sideLinks = document.querySelectorAll('.docs-side a');
  if (steps.length && sideLinks.length && 'IntersectionObserver' in window){
    const byId = {};
    sideLinks.forEach(a => { byId[a.getAttribute('href').slice(1)] = a; });
    const io2 = new IntersectionObserver((entries) => {
      entries.forEach(e => {
        const link = byId[e.target.id];
        if (!link) return;
        if (e.isIntersecting) {
          sideLinks.forEach(a => a.classList.remove('active'));
          link.classList.add('active');
        }
      });
    }, { rootMargin: '-15% 0px -70% 0px', threshold: 0 });
    steps.forEach(s => io2.observe(s));
  }
});
