'use strict';
// The IPC protocol of the protected-playback helper, defined once in protocol.json (Noctara's C++ side reads the
// same file). Messages are single-line JSON objects with a "type"; validate() checks them against the definition.
const definition = require('./protocol.json');

const VERSION = definition.version;

function validate(direction, message) {
  if (message === null || typeof message !== 'object' || Array.isArray(message)) {
    return 'not an object';
  }
  const fields = definition[direction][message.type];
  if (fields === undefined) {
    return `unknown message type ${JSON.stringify(message.type)}`;
  }
  for (const [name, spec] of Object.entries(fields)) {
    const optional = spec.endsWith('?');
    const type = optional ? spec.slice(0, -1) : spec;
    const value = message[name];
    if (value === undefined) {
      if (!optional) return `${message.type}: missing ${name}`;
      continue;
    }
    const actual = Array.isArray(value) ? 'array' : value === null ? 'null' : typeof value;
    if (actual !== type) return `${message.type}: ${name} must be a ${type}`;
  }
  return null;
}

module.exports = { VERSION, validate, definition };
