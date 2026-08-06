/**
 * ATLAS Web Animations & Interactions
 * Includes canvas-based Magic Cursor particles, Glideshow transitions, and interactive UI micro-behaviors.
 */

document.addEventListener('DOMContentLoaded', () => {
  // 1. Magic Cursor Particle Trail
  const initMagicCursor = () => {
    let canvas = document.getElementById('magicCursorCanvas');
    if (!canvas) {
      canvas = document.createElement('canvas');
      canvas.id = 'magicCursorCanvas';
      document.body.appendChild(canvas);
    }
    const ctx = canvas.getContext('2d');
    
    let width = canvas.width = window.innerWidth;
    let height = canvas.height = window.innerHeight;
    
    window.addEventListener('resize', () => {
      width = canvas.width = window.innerWidth;
      height = canvas.height = window.innerHeight;
    });

    const particles = [];
    const maxParticles = 60;
    const mouse = { x: 0, y: 0, active: false, speed: 0 };
    let lastMouse = { x: 0, y: 0 };

    window.addEventListener('mousemove', (e) => {
      mouse.x = e.clientX;
      mouse.y = e.clientY;
      mouse.active = true;

      // Calculate speed for particle sizing
      const dx = mouse.x - lastMouse.x;
      const dy = mouse.y - lastMouse.y;
      mouse.speed = Math.sqrt(dx*dx + dy*dy);
      
      lastMouse.x = mouse.x;
      lastMouse.y = mouse.y;

      // Add particles
      if (particles.length < maxParticles) {
        // Emerald to Cyan palette
        const hue = Math.random() > 0.5 ? 160 : 190; // 160 = Emerald green, 190 = Cyan
        particles.push({
          x: mouse.x,
          y: mouse.y,
          size: Math.random() * 4 + 2 + Math.min(mouse.speed * 0.15, 6),
          color: `hsla(${hue}, 85%, 60%, ${Math.random() * 0.4 + 0.3})`,
          alpha: 1,
          decay: Math.random() * 0.02 + 0.015,
          vx: (Math.random() - 0.5) * 1.5,
          vy: (Math.random() - 0.5) * 1.5 - 0.5 // slight upward drift
        });
      }
    });

    window.addEventListener('mouseleave', () => {
      mouse.active = false;
    });

    const animateParticles = () => {
      ctx.clearRect(0, 0, width, height);

      // Render glowing trail on mouse pointer itself (if active)
      if (mouse.active) {
        const gradient = ctx.createRadialGradient(mouse.x, mouse.y, 0, mouse.x, mouse.y, 25);
        gradient.addColorStop(0, 'rgba(6, 182, 212, 0.15)');
        gradient.addColorStop(1, 'rgba(6, 182, 212, 0)');
        ctx.fillStyle = gradient;
        ctx.beginPath();
        ctx.arc(mouse.x, mouse.y, 25, 0, Math.PI * 2);
        ctx.fill();
      }

      for (let i = particles.length - 1; i >= 0; i--) {
        const p = particles[i];
        p.x += p.vx;
        p.y += p.vy;
        p.alpha -= p.decay;

        if (p.alpha <= 0) {
          particles.splice(i, 1);
          continue;
        }

        ctx.save();
        ctx.globalAlpha = p.alpha;
        ctx.shadowBlur = 10;
        ctx.shadowColor = p.color;
        ctx.fillStyle = p.color;
        ctx.beginPath();
        ctx.arc(p.x, p.y, p.size, 0, Math.PI * 2);
        ctx.fill();
        ctx.restore();
      }
      requestAnimationFrame(animateParticles);
    };
    
    animateParticles();
  };

  initMagicCursor();

  // 2. Glideshow Slider Logic
  const initGlideshow = () => {
    const container = document.querySelector('.glideshow-container');
    if (!container) return;

    const wrapper = container.querySelector('.glideshow-wrapper');
    const slides = container.querySelectorAll('.glideshow-slide');
    const dotsContainer = container.querySelector('.glideshow-dots');
    const prevBtn = container.querySelector('.glideshow-btn.prev');
    const nextBtn = container.querySelector('.glideshow-btn.next');

    let currentIndex = 0;
    const slideCount = slides.length;
    let autoplayInterval;

    // Create Navigation Dots
    dotsContainer.innerHTML = '';
    for (let i = 0; i < slideCount; i++) {
      const dot = document.createElement('div');
      dot.classList.add('glideshow-dot');
      if (i === 0) dot.classList.add('active');
      dot.addEventListener('click', () => {
        goToSlide(i);
        resetAutoplay();
      });
      dotsContainer.appendChild(dot);
    }
    const dots = dotsContainer.querySelectorAll('.glideshow-dot');

    const updateSlideClasses = () => {
      slides.forEach((slide, idx) => {
        slide.classList.toggle('active', idx === currentIndex);
      });
      dots.forEach((dot, idx) => {
        dot.classList.toggle('active', idx === currentIndex);
      });
    };

    const goToSlide = (index) => {
      currentIndex = (index + slideCount) % slideCount;
      wrapper.style.transform = `translateX(-${currentIndex * 100}%)`;
      updateSlideClasses();
    };

    const nextSlide = () => goToSlide(currentIndex + 1);
    const prevSlide = () => goToSlide(currentIndex - 1);

    if (prevBtn) prevBtn.addEventListener('click', () => { prevSlide(); resetAutoplay(); });
    if (nextBtn) nextBtn.addEventListener('click', () => { nextSlide(); resetAutoplay(); });

    // Autoplay Timer
    const startAutoplay = () => {
      autoplayInterval = setInterval(nextSlide, 6000);
    };

    const resetAutoplay = () => {
      clearInterval(autoplayInterval);
      startAutoplay();
    };

    // Initialize slide classes
    goToSlide(0);
    startAutoplay();
  };

  initGlideshow();

  // 3. Scroll Reveal (Intersection Observer)
  const revealElements = document.querySelectorAll('.workbench, .card, .feature-card, .pipeline-step, .roadmap-item, .docs-content section, .reviews-dashboard, .review-card, .glideshow-container');
  
  const revealObserver = new IntersectionObserver((entries) => {
    entries.forEach(entry => {
      if (entry.isIntersecting) {
        entry.target.classList.add('reveal');
        revealObserver.unobserve(entry.target);
      }
    });
  }, {
    threshold: 0.05,
    rootMargin: '0px 0px -40px 0px'
  });

  revealElements.forEach(el => {
    el.style.opacity = '0'; // hide initially for smooth fade in
    revealObserver.observe(el);
  });

  // 4. Docs Sidebar Tracking
  const docsSections = document.querySelectorAll('.docs-content section');
  const sidebarLinks = document.querySelectorAll('.docs-sidebar a');

  if (docsSections.length > 0 && sidebarLinks.length > 0) {
    const activeObserver = new IntersectionObserver((entries) => {
      entries.forEach(entry => {
        if (entry.isIntersecting) {
          const id = entry.target.getAttribute('id');
          sidebarLinks.forEach(link => {
            link.classList.toggle('active', link.getAttribute('href') === `#${id}`);
          });
        }
      });
    }, {
      threshold: 0.3,
      rootMargin: '-10% 0px -60% 0px'
    });

    docsSections.forEach(section => activeObserver.observe(section));
  }

  // 5. Custom Pretty Buttons Interactivity (Magnetic/Sheen)
  const buttons = document.querySelectorAll('.button, button:not(.tab)');
  buttons.forEach(btn => {
    btn.addEventListener('mousemove', (e) => {
      const rect = btn.getBoundingClientRect();
      const x = e.clientX - rect.left;
      const y = e.clientY - rect.top;

      // Dynamic glow background positioning for buttons
      btn.style.setProperty('--x', `${x}px`);
      btn.style.setProperty('--y', `${y}px`);
    });
  });
});
