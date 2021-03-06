% Generated by roxygen2: do not edit by hand
% Please edit documentation in R/rxsympy.R
\name{rxSymPyExec}
\alias{rxSymPyExec}
\title{Execute Python statement without getting the return value.}
\usage{
rxSymPyExec(..., .python, .start = TRUE)
}
\arguments{
\item{...}{Parameters sent to Jython to execute.}

\item{.python}{Python to use, defaults to the started python.}

\item{.start}{A boolean (default TRUE) starting python first if
RxODE doesn't know it has been started...}
}
\value{
Nothing
}
\description{
Execute Python statement without getting the return value.
}
\author{
Matthew L. Fidler
}
\keyword{internal}
