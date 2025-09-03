function shouldBe(actual, expected) {
    if (Number.isNaN(expected)) {
        if (!Number.isNaN(actual))
            throw new Error(`Actual: ${actual}, Expected: ${expected}`);
    } else if (actual !== expected)
        throw new Error(`Actual: ${actual}, Expected: ${expected}`);
}

for (let year of Array(100).keys()) {
    for (let month of Array(12).keys()) {
        for (let day of Array(31).keys()) {
            let fullYear = year >= 50 ? year + 1900 : year + 2000;
            let fullDate = new Date(`${month + 1}/${day + 1}/${fullYear}`);

            // mm/dd/yy
            let d1 = new Date(`${month + 1}/${day + 1}/${year}`);
            shouldBe(d1.getTime(), fullDate.getTime())

            // yy/mm/dd
            let d2 = new Date(`${year}/${month + 1}/${day + 1}`);
            if (year > 31) {
                shouldBe(d2.getTime(), fullDate.getTime())
            } else if (year > 12) {
                shouldBe(d2.getTime(), new Date(NaN).getTime())
            }
        }
    }
}

shouldBe(new Date("99/1/99").getTime(), new Date(NaN).getTime());
shouldBe(new Date("13/13/13").getTime(), new Date(NaN).getTime());
shouldBe(new Date("0/10/0").getTime(), new Date(NaN).getTime());

// Written months.
// TODO: uncomment commented assertions when bug in date constructor is fixed
for (let year of Array(1000).keys()) {
    let fullDate = new Date(`5/1/${year}`);
    let d1 = new Date(`may 1 ${year}`);
    let d1_1 = new Date(`mayrandomstring 1 ${year}`);
    // let d2 = new Date(`1 may ${year}`);
    // let d3 = new Date(`1 ${year} may`);

    shouldBe(d1.getTime(), fullDate.getTime())
    shouldBe(d1_1.getTime(), fullDate.getTime())
    // shouldBe(d2.getTime(), fullDate.getTime())
    // shouldBe(d3.getTime(), fullDate.getTime())

    // if (year > 31) {
    //   let d4 = new Date(`may ${year} 1`);
    //   let d4_1 = new Date(`mayrandomstring ${year} 1`);
    //   let d5 = new Date(`${year} may 1`);
    //   let d6 = new Date(`${year} 1 may`);

    //   shouldBe(d4.getTime(), fullDate.getTime())
    //   shouldBe(d4_1.getTime(), fullDate.getTime())
    //   shouldBe(d5.getTime(), fullDate.getTime())
    //   shouldBe(d6.getTime(), fullDate.getTime())
    // }
}

shouldBe(new Date("may 1999 1999").getTime(), new Date(NaN).getTime());
shouldBe(new Date("may 0 0").getTime(), new Date(NaN).getTime());
