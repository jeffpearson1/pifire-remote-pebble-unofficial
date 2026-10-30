// PebbleKit JS side: talks HTTP to PiFire and relays status to the watch.
//
// PiFire endpoints used (v1.7+):
//   GET /api/current                    status, temps, notify targets
//   GET /api/set/mode/{mode}            startup, smoke, shutdown, stop, monitor, reignite
//   GET /api/set/mode/hold/{temp}       hold at a setpoint
//   GET /api/set/splus/{true|false}     smoke plus

var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

var REQUEST_TIMEOUT_MS = 8000;
var MAX_PROBES = 3;
var MAX_LABEL_LEN = 10;

var MODE_COMMANDS = ['startup', 'smoke', 'shutdown', 'stop', 'monitor', 'reignite'];

function baseUrl() {
  var settings = {};
  try {
    settings = JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (e) {
    settings = {};
  }
  var url = String(settings.PIFIRE_URL || '').trim();
  if (!url) {
    return null;
  }
  if (!/^https?:\/\//i.test(url)) {
    url = 'http://' + url;
  }
  return url.replace(/\/+$/, '');
}

function sendToWatch(msg) {
  Pebble.sendAppMessage(msg, null, function (e) {
    console.log('sendAppMessage failed: ' + JSON.stringify(e));
  });
}

function sendError(text) {
  sendToWatch({ ERROR: String(text).substring(0, 46) });
}

function getJson(path, onSuccess, onFailure) {
  var base = baseUrl();
  if (!base) {
    onFailure('Set PiFire address');
    return;
  }
  var xhr = new XMLHttpRequest();
  xhr.open('GET', base + path, true);
  xhr.timeout = REQUEST_TIMEOUT_MS;
  xhr.onload = function () {
    if (xhr.status < 200 || xhr.status >= 300) {
      onFailure('HTTP ' + xhr.status);
      return;
    }
    var data;
    try {
      data = JSON.parse(xhr.responseText);
    } catch (e) {
      onFailure('Bad response from PiFire');
      return;
    }
    onSuccess(data);
  };
  xhr.onerror = function () { onFailure('Cannot reach PiFire'); };
  xhr.ontimeout = function () { onFailure('PiFire timed out'); };
  xhr.send();
}

function formatTemp(value) {
  if (value === null || value === undefined || isNaN(value) || value <= 0) {
    return '--';
  }
  return String(Math.round(value));
}

function firstValue(obj) {
  for (var key in obj) {
    if (obj.hasOwnProperty(key)) {
      return obj[key];
    }
  }
  return null;
}

// Fit a probe name in MAX_LABEL_LEN without losing a trailing number:
// "Food Probe 1" -> "FoodProbe1" rather than "Food Probe".
function shortName(name) {
  name = String(name);
  if (name.length > MAX_LABEL_LEN) {
    name = name.replace(/\s+/g, '');
  }
  if (name.length > MAX_LABEL_LEN) {
    var suffix = (name.match(/\d+$/) || [''])[0];
    name = name.substring(0, MAX_LABEL_LEN - suffix.length) + suffix;
  }
  return name;
}

// "Brisket 165° (203)" per line; the target shows only when a notification is armed.
function formatProbes(foodTemps, notifyData) {
  var targets = {};
  var names = {};
  (notifyData || []).forEach(function (n) {
    if (!n || !n.label) {
      return;
    }
    if (n.name) {
      names[n.label] = n.name;
    }
    // PiFire also lists probe_limit_high/low alerts per probe; only "probe" is the cook target.
    if ((!n.type || n.type === 'probe') && n.req && n.target) {
      targets[n.label] = n.target;
    }
  });

  var lines = [];
  for (var label in foodTemps) {
    if (!foodTemps.hasOwnProperty(label) || lines.length >= MAX_PROBES) {
      continue;
    }
    var name = shortName(names[label] || label);
    var temp = formatTemp(foodTemps[label]);
    var line = name + ' ' + temp + (temp === '--' ? '' : '°');
    if (targets[label]) {
      line += ' (' + Math.round(targets[label]) + ')';
    }
    lines.push(line);
  }
  return lines.length ? lines.join('\n') : 'No food probes';
}

function statusLine(status) {
  if (status.critical_error) {
    return 'CRITICAL ERROR';
  }
  if (status.lid_open_detected) {
    return 'Lid open';
  }
  return String(status.name || 'PiFire').substring(0, 30);
}

function refresh() {
  getJson('/api/current', function (data) {
    var current = data.current || {};
    var status = data.status || {};
    sendToWatch({
      MODE: String(status.mode || 'Unknown').substring(0, 15),
      GRILL_TEMP: formatTemp(firstValue(current.P || {})),
      SETPOINT: Math.round(current.PSP || 0),
      UNITS: status.units === 'C' ? 'C' : 'F',
      PROBES: formatProbes(current.F || {}, data.notify_data),
      SPLUS: status.s_plus ? 1 : 0,
      STATUS: statusLine(status)
    });
  }, sendError);
}

function runCommand(path) {
  getJson(path, function (data) {
    if (data && data.result === 'ERROR') {
      sendError(data.message || 'PiFire rejected command');
    }
    refresh();
  }, sendError);
}

function handleCommand(cmd, arg) {
  switch (cmd) {
    case 'refresh':
      refresh();
      break;
    case 'mode':
      var mode = String(arg || '').toLowerCase();
      if (MODE_COMMANDS.indexOf(mode) === -1) {
        sendError('Unknown mode ' + arg);
        return;
      }
      runCommand('/api/set/mode/' + mode);
      break;
    case 'hold':
      var temp = parseInt(arg, 10);
      if (isNaN(temp)) {
        sendError('Bad setpoint');
        return;
      }
      runCommand('/api/set/mode/hold/' + temp);
      break;
    case 'splus':
      runCommand('/api/set/splus/' + (arg === 'true' ? 'true' : 'false'));
      break;
    default:
      console.log('Unknown command: ' + cmd);
  }
}

Pebble.addEventListener('ready', function () {
  sendToWatch({ READY: 1 });
});

Pebble.addEventListener('appmessage', function (e) {
  var payload = e.payload || {};
  if (payload.CMD) {
    handleCommand(payload.CMD, payload.ARG);
  }
});
