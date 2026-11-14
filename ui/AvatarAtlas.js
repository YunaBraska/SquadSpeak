.pragma library

const speechHold = 240

// One physical row per state, with one or more clips and their timing in ms.
function layout(edges, stride = 229, frames = [2, 2, 2]) {
    return { stride: stride, rows: edges.slice(0, -1).map(function(top, row) {
        return { top: top, bottom: edges[row + 1], frames: frames,
                 duration: row === 1 ? 560 : 700,
                 repeats: row === 1 ? 3 : 1,
                 pause: row === 1 ? 0 : 3800 }
    }) }
}

const regular = layout([0, 229, 458, 687, 916, 1145])
const atlases = {
    mossling: layout([0, 112, 224, 336, 448, 560], 122, [8, 8, 8]),
    courier: layout([0, 123, 246, 369, 492, 615], 120, [8, 8, 8]),
    "ember-dragon": layout([0, 118, 236, 354, 472, 590], 117, [8, 8, 8]),
    mechanic: layout([0, 131, 262, 393, 524, 655], 128, [8, 8, 8]),
    "moon-owl": layout([0, 124, 248, 372, 496, 620], 127, [8, 8, 8]),
    "mushroom-sage": layout([0, 136, 272, 408, 544, 680], 140, [7, 7, 7]),
    "coral-diver": layout([0, 173, 346, 519, 692, 865], 168, [4, 4, 4]),
    "clockwork-beetle": layout([0, 176, 352, 528, 704, 880], 175, [4, 4, 4]),
    "crystal-golem": layout([0, 167, 334, 501, 668, 835], 189, [4, 4, 4]),
    "cloud-shepherd": layout([0, 164, 328, 492, 656, 820], 185, [4, 4, 4]),
    "star-moth": layout([0, 174, 348, 522, 696, 870], 193, [4, 3, 4]),
    "lantern-ghost": layout([0, 175, 350, 525, 700, 875], 175, [4, 4, 4]),
    "desert-fennec": layout([0, 176, 352, 528, 704, 880], 170, [4, 4, 4]),
    "octopus-pilot": layout([0, 171, 342, 513, 684, 855], 169, [4, 5, 4]),
    "snail-captain": layout([0, 175, 350, 525, 700, 875], 188, [4, 4, 4]),
    "orchid-mantis": layout([0, 174, 348, 522, 696, 870], 188, [4, 4, 4]),
    "comet-axolotl": layout([0, 176, 352, 528, 704, 880], 179, [4, 4, 4]),
    "frost-yeti": layout([0, 168, 336, 504, 672, 840], 183, [4, 4, 4]),
    "raccoon-alchemist": layout([0, 167, 334, 501, 668, 835], 182, [4, 4, 4]),
    "sunflower-knight": layout([0, 178, 356, 534, 712, 890], 169, [4, 4, 4]),
    system: layout([0, 121, 242, 363, 484, 605], 120, [8, 8, 8])
}
atlases.mossling.rows[3].frames = [8, 8, 16]
atlases.mechanic.rows[3].frames = [8, 4, 4]
atlases["mushroom-sage"].rows[3].frames = [7, 3, 4]
atlases["mushroom-sage"].rows[4].frames = [7, 3, 4]
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
    return Qt.rect(column * atlas.stride, row.top, atlas.stride, row.bottom - row.top)
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
