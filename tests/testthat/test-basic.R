library(RxODE)
context("Basic Tests")
rxPermissive({
    test.dir <- tempfile("Rx_base-")
    dir.create(test.dir)
    ode <- 'd/dt(y) = r * y * (1.0 - y/K);'
    fn <- file.path(test.dir, "exam3.1.txt")
    writeLines(ode, fn)
    m1 <- RxODE(model = ode, modName = "m1", do.compile=FALSE)

    test_that("ODE inside a string",{
        expect_equal(class(m1),"RxODE");
    })

    m2 <- RxODE(filename = fn, modName = "f1", do.compile=FALSE)
    test_that("ODE inside a text file",{
        expect_equal(class(m2),"RxODE");
    })

    test_that("arguments model= and filename= are mutually exclusive.",{
        expect_error(RxODE(model=ode, filename=fn, do.compile=FALSE),"Must specify exactly one of 'model' or 'filename'.");
    })

    unlink(test.dir, recursive = TRUE)
}, silent=TRUE, cran=TRUE)
