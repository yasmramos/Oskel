// Benchmark: build n short strings, join them with a comma, split them back and
// sum their lengths. Same algorithm as `strings.oskl`.
'use strict';

const n = 5000;
const parts = [];
for (let i = 0; i < n; i++) parts.push(`item-${i}`);
const text = parts.join(',');
const words = text.split(',');
let chars = 0;
for (const w of words) chars += w.length;
console.log(`strings ${text.length} ${words.length} ${chars}`);
