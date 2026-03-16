(function() {
  var COLORS = ['#0d6efd','#dc3545','#198754','#fd7e14','#6f42c1',
                '#d63384','#20c997','#0dcaf0','#ffc107','#6610f2'];

  var HOVER_STYLE = {
    bgcolor: '#fff', bordercolor: '#dee2e6', font: {color: '#212529'}
  };

  function buildTraces(series) {
    return series.map(function(s, i) {
      return {
        x: s.d.map(function(p){ return p[0]; }),
        y: s.d.map(function(p){ return p[1]; }),
        type: 'scattergl', mode: 'lines', name: s.n,
        line: {color: COLORS[i % COLORS.length], width: 1.5}
      };
    });
  }

  // Render all inline Plotly charts
  document.querySelectorAll('div[data-plotly]').forEach(function(el) {
    var info = JSON.parse(el.getAttribute('data-plotly'));
    var layout = {
      margin: {l:55, r:15, t:10, b:40},
      xaxis: {title: 'Time (s)', type: 'linear',
              rangeslider: {visible: !!info.rs}},
      yaxis: {title: info.y},
      legend: {orientation: 'h', y: -0.2},
      hovermode: 'x unified',
      hoverlabel: HOVER_STYLE,
      paper_bgcolor: 'transparent', plot_bgcolor: '#fff'
    };
    Plotly.newPlot(el, buildTraces(info.s), layout,
      {responsive: true, displaylogo: false,
       modeBarButtonsToRemove: ['lasso2d','select2d']});
  });

  // Modal logic
  var modal = document.getElementById('plot-modal');
  if (modal) {
    function closeModal() {
      modal.style.display = 'none';
      Plotly.purge('modal-plot');
    }
    modal.addEventListener('click', function(e) {
      if (e.target === modal) closeModal();
    });
    modal.querySelector('.modal-close').addEventListener('click', closeModal);
    document.addEventListener('keydown', function(e) {
      if (e.key === 'Escape') closeModal();
    });
  }

  window.showPlot = function(key) {
    var data = window.PLOT_DATA && window.PLOT_DATA[key];
    if (!data) return;
    document.getElementById('modal-title').textContent = data.t;
    modal.style.display = 'flex';
    var layout = {
      margin: {l:60, r:20, t:10, b:45},
      xaxis: {title: 'Time (s)', type: 'linear',
              rangeslider: {visible: true}},
      yaxis: {title: data.y},
      legend: {orientation: 'h', y: -0.12},
      hovermode: 'x unified',
      hoverlabel: HOVER_STYLE,
      paper_bgcolor: 'transparent', plot_bgcolor: '#fff'
    };
    Plotly.newPlot('modal-plot', buildTraces(data.s), layout,
      {responsive: true, displaylogo: false,
       modeBarButtonsToRemove: ['lasso2d','select2d']});
  };
})();
