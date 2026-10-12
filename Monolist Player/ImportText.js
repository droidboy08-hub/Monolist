.pragma library

// How an import's progress is told (Importer.job, Importer.jobInfo), in the
// same words wherever it shows: the import's window, the sidebar and its
// rail, and the imported playlist's page.

// The songs looked for so far, whatever became of them.
function settled(job) {
    return job.found + job.likely + job.check + job.notFound + job.dismissed
}

// Into the playlist: the sure, the likely and the ones the user chose.
function added(job) {
    return job.found + job.likely
}

function progress(job) {
    return job.total > 0 ? Math.min(1, settled(job) / job.total) : 0
}

// Asking YouTube Music right now, rather than resting, waiting or paused.
function running(job) {
    return job.state === "reading" || job.state === "finding"
}

// Not done and not stopped: there is progress to show.
function active(job) {
    return job.id > 0 && !job.done
}

// "14:35", in the system's own way of writing a time; "Sun 14:35" when it
// is not today (a day's cap, a long pause). The engine's own words for it
// (job.resumeText) where it gives them, so every screen says the same.
function clock(ms) {
    const at = new Date(ms)
    const time = Qt.formatTime(at, Qt.locale().timeFormat(1))
    return at.toDateString() === new Date().toDateString() ? time : Qt.formatDate(at, "ddd") + " " + time
}

function resumeTime(job) {
    return job.resumeText ? job.resumeText : clock(job.resumeAt)
}

// What it is doing, for a line of small capitals: "FINDING SONGS · 42 OF 100".
function stateLine(job) {
    const of = settled(job) + " OF " + job.total
    switch (job.state) {
    case "reading": return "READING THE LIST · " + job.read + " SONGS SO FAR"
    case "finding": return "FINDING SONGS · " + of
    case "queued": return "QUEUED · " + of
    case "resting": return "RESTING UNTIL " + resumeTime(job).toUpperCase() + " · " + of
    case "waiting": return "WAITING UNTIL " + resumeTime(job).toUpperCase() + " · " + of
    case "paused": return (job.resumeAt > 0 ? "PAUSED UNTIL " + resumeTime(job).toUpperCase() : "PAUSED") + " · " + of
    case "done": return "IMPORTED · " + added(job) + " OF " + job.total + " IN THE PLAYLIST"
    case "cancelled": return "STOPPED · " + added(job) + " OF " + job.total + " IN THE PLAYLIST"
    }
    return ""
}

// How long, roughly: "4 minutes", "2 hours", "3 days". Under a minute, "a
// minute".
function duration(seconds) {
    const minutes = Math.max(1, Math.round(seconds / 60))
    if (minutes < 90)
        return minutes === 1 ? "a minute" : minutes + " minutes"
    const hours = Math.round(minutes / 60)
    if (hours < 36)
        return hours + " hours"
    return Math.round(hours / 24) + " days"
}

// A rough guess, said as one: "About 4 minutes left". The hour's and the
// day's searches are in it (Importer), so a long list says days.
function timeLeft(seconds) {
    if (seconds < 60)
        return "Less than a minute left"
    const text = duration(seconds)
    return (text === "a minute" ? "About a minute" : "About " + text) + " left"
}

// The sentence under the counts: how long, or why it is not going on.
function noteLine(job) {
    switch (job.state) {
    case "reading":
        // A YouTube list's songs are its own: no searching, only its pages.
        if (job.platform === "ytmusic" || job.platform === "youtube")
            return "Reading the list a page at a time; its songs go straight into the playlist."
        return timeLeft(job.secondsLeft)
    case "finding": return timeLeft(job.secondsLeft)
    case "queued": return "It starts when the import before it is done."
    case "resting":
    case "waiting":
    case "paused": return job.note
    case "done": {
        const left = []
        if (job.check > 0)
            left.push(job.check + " to check")
        if (job.notFound > 0)
            left.push(job.notFound + " not found")
        return left.length > 0 ? "The " + left.join(" and ") + " wait for you on the playlist's page."
                               : "Every song was found."
    }
    case "cancelled": return job.note
    }
    return ""
}

// One song's line in the list: what became of it. `searching`: the one
// being looked for now.
function statusLabel(status, reason, searching) {
    switch (status) {
    case "pending": return searching ? "LOOKING" : ""
    case "found": return reason === "In your library already" ? "IN LIBRARY" : "FOUND"
    case "likely": return "LIKELY"
    case "check": return "TO CHECK"
    case "notfound": return "NOT FOUND"
    case "dismissed": return "DISMISSED"
    }
    return ""
}

// "3:45"; empty for no length.
function length(ms) {
    if (!(ms > 0))
        return ""
    const seconds = Math.round(ms / 1000)
    const rest = seconds % 60
    return Math.floor(seconds / 60) + ":" + (rest < 10 ? "0" : "") + rest
}

function songs(n) {
    return n + (n === 1 ? " song" : " songs")
}

// The matcher's reasons are phrases ("no length to compare"): printed as a
// sentence of their own.
function sentence(text) {
    if (!text)
        return ""
    const capital = text.charAt(0).toUpperCase() + text.substring(1)
    return /[.!?]$/.test(capital) ? capital : capital + "."
}
