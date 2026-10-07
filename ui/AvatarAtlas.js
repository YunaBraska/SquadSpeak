.pragma library

// One physical row per state, with one or more clips and their timing in ms.
function layout(edges, inset, stride = 229, frames = [2, 2, 2]) {
    return { stride: stride, inset: inset, rows: edges.slice(0, -1).map(function(top, row) {
        return { top: top, bottom: edges[row + 1], frames: frames,
                 duration: row === 1 ? 560 : 700,
                 repeats: row === 1 ? 3 : 1,
                 pause: row === 1 ? 0 : 3800 }
    }) }
}

const regular = layout([0, 229, 458, 687, 916, 1145], 0)
const atlases = {
    mossling: layout([0, 108, 216, 324, 432, 540], 0, 113, [8, 8, 8]),
    courier: layout([0, 113, 225, 336, 449, 555], 4, 111, [8, 8, 8]),
    "ember-dragon": layout([0, 109, 216, 325, 432, 541], 0, 109, [8, 8, 8]),
    mechanic: layout([0, 229, 458, 687, 916, 1145], 4),
    system: layout([0, 119, 238, 357, 476, 595], 4, 111, [8, 8, 8])
}
atlases.mossling.rows[3].frames = [8, 8, 16]
atlases.system.rows[2].frames = [8, 4, 4]
atlases.system.rows.forEach(function(row) {
    row.duration = 600; row.repeats = 1; row.pause = 11400
})

function forAvatar(id, system) { return system ? atlases.system : atlases[id] || regular }

function frameInterval(layouts) {
    let interval = 125
    for (const atlas of layouts || [regular].concat(Object.values(atlases)))
        for (const row of atlas.rows)
            interval = Math.min(interval, row.duration / Math.max.apply(null, row.frames) / 2)
    return Math.max(16, Math.floor(interval))
}

function frameRect(atlas, row, column) {
    return Qt.rect(column * atlas.stride + atlas.inset, row.top + atlas.inset,
                   atlas.stride - 2 * atlas.inset, row.bottom - row.top - 2 * atlas.inset)
}

function random(seed, cycle, salt) {
    let value = (seed ^ Math.imul(cycle, 0x9e3779b1) ^ salt) >>> 0
    value = Math.imul(value ^ (value >>> 16), 0x21f0aaad)
    value = Math.imul(value ^ (value >>> 15), 0x735a2d97)
    return (value ^ (value >>> 15)) >>> 0
}

function idleColumn(row, cycle, phase, seed) {
    const count = row.frames.reduce(function(sum, frames) { return sum + frames }, 0)
    const start = random(seed, cycle - 1, 1) % count
    const end = random(seed, cycle, 1) % count
    let distance = end - start
    if (distance > count / 2) distance -= count
    if (distance < -count / 2) distance += count
    if (distance === 0) return start
    const delay = random(seed, cycle, 2) % (row.pause + 1)
    const duration = row.duration * Math.min(1, Math.abs(distance) / Math.max.apply(null, row.frames))
    const progress = Math.max(0, Math.min(1, (phase - delay) / duration))
    // Follow adjacent drawn poses, then hold the destination until the next
    // movement. Identity and shared time keep all views of a member in sync.
    return (start + Math.sign(distance) * Math.floor(Math.abs(distance) * progress) + count) % count
}
