/* materialize.hpp — fill flat declarative model instances from row tuples. */
#pragma once

#include <Python.h>

/* materialize(model, keys, rows) -> list. Registered on fastpostgres._client. */
bool fp_register_materialize(PyObject *module);
