/* materialize.cpp — fill flat declarative model instances from row tuples.
 *
 * SQLAlchemy's ORM hydration is per attribute, through the instrumented
 * descriptor, with an InstanceState, a weakref, and the identity map on the
 * side. For a flat model whose columns are already in the row frame, all of
 * that is bookkeeping the read does not need: allocate the instance, give it
 * the column values in its ``__dict__`` — which is exactly where the
 * descriptor looks first — and stop. No ``__init__``, no events, no session.
 *
 * The contract is deliberately narrow; orm.py's docstring states what a
 * materialized instance cannot do. Keys are the model's attribute keys, in the
 * same order as the row tuples. This kernel is only reached after orm.py has
 * proved the model is flat; a mapper with relationships, inheritance,
 * validators, deferred columns, composites or versioning is materialized as
 * stock SQLAlchemy instances in Python instead.
 */
#include "materialize.hpp"

/* Rows are the driver's tuples (or lists), which is the fast path; any other
 * sequence is converted once per row. */
static PyObject *
materialize_materialize(PyObject *self, PyObject *args)
{
    PyObject *model, *keys, *rows;

    if (!PyArg_ParseTuple(args, "OOO:materialize", &model, &keys, &rows)) return nullptr;
    if (!PyType_Check(model))
    {
        PyErr_SetString(PyExc_TypeError, "model must be a class");
        return nullptr;
    }

    PyObject *keyseq = PySequence_Fast(keys, "keys must be a sequence");
    if (keyseq == nullptr) return nullptr;
    PyObject *rowseq = PySequence_Fast(rows, "rows must be a sequence");
    if (rowseq == nullptr)
    {
        Py_DECREF(keyseq);
        return nullptr;
    }

    Py_ssize_t nkeys = PySequence_Fast_GET_SIZE(keyseq);
    Py_ssize_t nrows = PySequence_Fast_GET_SIZE(rowseq);
    PyObject **keyv = PySequence_Fast_ITEMS(keyseq);
    PyTypeObject *tp = (PyTypeObject *)model;

    if (tp->tp_alloc == nullptr || tp->tp_dictoffset == 0)
    {
        Py_DECREF(keyseq);
        Py_DECREF(rowseq);
        PyErr_SetString(PyExc_TypeError,
                        "materialize() needs a model with a __dict__ (flat declarative models)");
        return nullptr;
    }
    PyObject *out = PyList_New(nrows);
    if (out == nullptr)
    {
        Py_DECREF(keyseq);
        Py_DECREF(rowseq);
        return nullptr;
    }

    for (Py_ssize_t i = 0; i < nrows; i++)
    {
        PyObject *row = PySequence_Fast_GET_ITEM(rowseq, i);
        PyObject *fast = nullptr;  /* only when the row is not a tuple/list */
        PyObject **rowv;
        Py_ssize_t nrow;

        if (PyTuple_Check(row))
        {
            rowv = ((PyTupleObject *)row)->ob_item;
            nrow = PyTuple_GET_SIZE(row);
        }
        else if (PyList_Check(row))
        {
            rowv = ((PyListObject *)row)->ob_item;
            nrow = PyList_GET_SIZE(row);
        }
        else
        {
            fast = PySequence_Fast(row, "each row must be a sequence");
            if (fast == nullptr) goto fail;
            rowv = PySequence_Fast_ITEMS(fast);
            nrow = PySequence_Fast_GET_SIZE(fast);
        }
        if (nrow != nkeys)
        {
            PyErr_SetString(PyExc_ValueError, "row length does not match keys");
            Py_XDECREF(fast);
            goto fail;
        }

        PyObject *obj = tp->tp_alloc(tp, 0);
        if (obj == nullptr)
        {
            Py_XDECREF(fast);
            goto fail;
        }

        PyObject *dict = PyObject_GenericGetDict(obj, nullptr);
        if (dict == nullptr)
        {
            Py_DECREF(obj);
            Py_XDECREF(fast);
            goto fail;
        }
        for (Py_ssize_t j = 0; j < nkeys; j++)
        {
            if (PyDict_SetItem(dict, keyv[j], rowv[j]) != 0)
            {
                Py_DECREF(dict);
                Py_DECREF(obj);
                Py_XDECREF(fast);
                goto fail;
            }
        }
        Py_DECREF(dict);
        Py_XDECREF(fast);

        PyList_SET_ITEM(out, i, obj);
    }

    Py_DECREF(keyseq);
    Py_DECREF(rowseq);
    return out;

fail:
    Py_DECREF(out);
    Py_DECREF(keyseq);
    Py_DECREF(rowseq);
    return nullptr;
}

static PyMethodDef materialize_methods[] = {
    {"materialize", materialize_materialize, METH_VARARGS,
     "materialize(model, keys, rows) -> list: flat model instances from row tuples"},
    {nullptr},
};

bool
fp_register_materialize(PyObject *module)
{
    for (PyMethodDef *m = materialize_methods; m->ml_name != nullptr; m++)
    {
        PyObject *fn = PyCFunction_NewEx(m, nullptr, nullptr);
        if (fn == nullptr) return false;
        if (PyModule_AddObject(module, m->ml_name, fn) != 0)
        {
            Py_DECREF(fn);
            return false;
        }
    }
    return true;
}
