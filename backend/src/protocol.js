import { readFileSync } from 'node:fs';

// This file is also consumed by CMake to generate native protocol constants.
export const contract = JSON.parse(readFileSync(new URL('../protocol/v3.json', import.meta.url), 'utf8'));
export const protocol = Object.freeze({ name: contract.name, major: contract.major, minor: contract.minor,
  features: Object.freeze([...contract.features]) });
export const limits = Object.freeze(contract.limits);
export function supportsVersion(value) { return value === undefined || value === String(protocol.major); }
