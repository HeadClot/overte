//
//  ScriptEngineQtScript_cast.cpp
//  libraries/script-engine/src/qtscript
//
//  Created by Heather Anderson 12/9/2021
//  Copyright 2021 Vircadia contributors.
//  Copyright 2022 Overte e.V.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ScriptEngineQtScript.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtScript/QScriptEngine>

#include "../ScriptEngineCast.h"
#include "../ScriptValueIterator.h"

#include "ScriptObjectQtProxy.h"
#include "ScriptValueQtWrapper.h"

void ScriptEngineQtScript::setDefaultPrototype(int metaTypeId, const ScriptValue& prototype) {
    ScriptValueQtWrapper* unwrappedPrototype = ScriptValueQtWrapper::unwrap(prototype);
    if (unwrappedPrototype) {
        const QScriptValue& scriptPrototype = unwrappedPrototype->toQtValue();
        _customTypeProtect.lockForWrite();
        _customPrototypes.insert(metaTypeId, scriptPrototype);
        _customTypeProtect.unlock();
    }
}

void ScriptEngineQtScript::registerCustomType(int type,
                                              ScriptEngine::MarshalFunction marshalFunc,
                                              ScriptEngine::DemarshalFunction demarshalFunc)
{
    _customTypeProtect.lockForWrite();

    // storing it in a map for our own benefit
    CustomMarshal& customType = _customTypes.insert(type, CustomMarshal()).value();
    customType.demarshalFunc = demarshalFunc;
    customType.marshalFunc = marshalFunc;
    _customTypeProtect.unlock();
}

Q_DECLARE_METATYPE(ScriptValue);

static QScriptValue ScriptValueToQScriptValue(QScriptEngine* engine, const ScriptValue& src) {
    return ScriptValueQtWrapper::fullUnwrap(static_cast<ScriptEngineQtScript*>(engine), src);
}

static void ScriptValueFromQScriptValue(const QScriptValue& src, ScriptValue& dest) {
    ScriptEngineQtScript* engine = static_cast<ScriptEngineQtScript*>(src.engine());
    dest = ScriptValue(new ScriptValueQtWrapper(engine, src));
}

static ScriptValue StringListToScriptValue(ScriptEngine* engine, const QStringList& src) {
    int len = src.length();
    ScriptValue dest = engine->newArray(len);
    for (int idx = 0; idx < len; ++idx) {
        dest.setProperty(idx, engine->newValue(src.at(idx)));
    }
    return dest;
}

static bool StringListFromScriptValue(const ScriptValue& src, QStringList& dest) {
    if(!src.isArray()) return false;
    int len = src.property("length").toInteger();
    dest.clear();
    for (int idx = 0; idx < len; ++idx) {
        dest.append(src.property(idx).toString());
    }
    return true;
}

static ScriptValue VariantListToScriptValue(ScriptEngine* engine, const QVariantList& src) {
    int len = src.length();
    ScriptValue dest = engine->newArray(len);
    for (int idx = 0; idx < len; ++idx) {
        dest.setProperty(idx, engine->newVariant(src.at(idx)));
    }
    return dest;
}

static bool VariantListFromScriptValue(const ScriptValue& src, QVariantList& dest) {
    if(!src.isArray()) return false;
    int len = src.property("length").toInteger();
    dest.clear();
    for (int idx = 0; idx < len; ++idx) {
        dest.append(src.property(idx).toVariant());
    }
    return true;
}

static ScriptValue VariantMapToScriptValue(ScriptEngine* engine, const QVariantMap& src) {
    ScriptValue dest = engine->newObject();
    for (QVariantMap::const_iterator iter = src.cbegin(); iter != src.cend(); ++iter) {
        dest.setProperty(iter.key(), engine->newVariant(iter.value()));
    }
    return dest;
}

static bool VariantMapFromScriptValue(const ScriptValue& src, QVariantMap& dest) {
    dest.clear();
    ScriptValueIteratorPointer iter = src.newIterator();
    while (iter->hasNext()) {
        iter->next();
        dest.insert(iter->name(), iter->value().toVariant());
    }
    return true;
}

static ScriptValue VariantHashToScriptValue(ScriptEngine* engine, const QVariantHash& src) {
    ScriptValue dest = engine->newObject();
    for (QVariantHash::const_iterator iter = src.cbegin(); iter != src.cend(); ++iter) {
        dest.setProperty(iter.key(), engine->newVariant(iter.value()));
    }
    return dest;
}

static bool VariantHashFromScriptValue(const ScriptValue& src, QVariantHash& dest) {
    dest.clear();
    ScriptValueIteratorPointer iter = src.newIterator();
    while (iter->hasNext()) {
        iter->next();
        dest.insert(iter->name(), iter->value().toVariant());
    }
    return true;
}

static ScriptValue JsonValueToScriptValue(ScriptEngine* engine, const QJsonValue& src) {
    return engine->newVariant(src.toVariant());
}

static bool JsonValueFromScriptValue(const ScriptValue& src, QJsonValue& dest) {
    dest = QJsonValue::fromVariant(src.toVariant());
    return true;
}

static ScriptValue JsonObjectToScriptValue(ScriptEngine* engine, const QJsonObject& src) {
    QVariantMap map = src.toVariantMap();
    ScriptValue dest = engine->newObject();
    for (QVariantMap::const_iterator iter = map.cbegin(); iter != map.cend(); ++iter) {
        dest.setProperty(iter.key(), engine->newVariant(iter.value()));
    }
    return dest;
}

static bool JsonObjectFromScriptValue(const ScriptValue& src, QJsonObject& dest) {
    QVariantMap map;
    ScriptValueIteratorPointer iter = src.newIterator();
    while (iter->hasNext()) {
        iter->next();
        map.insert(iter->name(), iter->value().toVariant());
    }
    dest = QJsonObject::fromVariantMap(map);
    return true;
}

static ScriptValue JsonArrayToScriptValue(ScriptEngine* engine, const QJsonArray& src) {
    QVariantList list = src.toVariantList();
    int len = list.length();
    ScriptValue dest = engine->newArray(len);
    for (int idx = 0; idx < len; ++idx) {
        dest.setProperty(idx, engine->newVariant(list.at(idx)));
    }
    return dest;
}

static bool JsonArrayFromScriptValue(const ScriptValue& src, QJsonArray& dest) {
    if(!src.isArray()) return false;
    QVariantList list;
    int len = src.property("length").toInteger();
    for (int idx = 0; idx < len; ++idx) {
        list.append(src.property(idx).toVariant());
    }
    dest = QJsonArray::fromVariantList(list);
    return true;
}

// QMetaType::QJsonArray

void ScriptEngineQtScript::registerSystemTypes() {
    qScriptRegisterMetaType(this, ScriptValueToQScriptValue, ScriptValueFromQScriptValue);

    scriptRegisterMetaType<QStringList, StringListToScriptValue, StringListFromScriptValue>(this);
    scriptRegisterMetaType<QVariantList, VariantListToScriptValue, VariantListFromScriptValue>(this);
    scriptRegisterMetaType<QVariantMap, VariantMapToScriptValue, VariantMapFromScriptValue>(this);
    scriptRegisterMetaType<QVariantHash, VariantHashToScriptValue, VariantHashFromScriptValue>(this);
    scriptRegisterMetaType<QJsonValue, JsonValueToScriptValue, JsonValueFromScriptValue>(this);
    scriptRegisterMetaType<QJsonObject, JsonObjectToScriptValue, JsonObjectFromScriptValue>(this);
    scriptRegisterMetaType<QJsonArray, JsonArrayToScriptValue, JsonArrayFromScriptValue>(this);
}

int ScriptEngineQtScript::computeCastPenalty(QScriptValue& val, int destTypeId) {
    if (val.isNumber()) {
        switch (destTypeId){
            case QMetaType::Bool:
                // Conversion to bool is acceptable, but numbers are preferred
                return 5;
                break;
            case QMetaType::UInt:
            case QMetaType::ULong:
            case QMetaType::Int:
            case QMetaType::Long:
            case QMetaType::Short:
            case QMetaType::Double:
            case QMetaType::Float:
            case QMetaType::ULongLong:
            case QMetaType::LongLong:
            case QMetaType::UShort:
                // Perfect case. JS doesn't have separate integer and floating point type
                return 0;
                break;
            case QMetaType::QString:
            case QMetaType::QByteArray:
            case QMetaType::QDateTime:
            case QMetaType::QDate:
                // Conversion to string should be avoided, it's probably not what we want
                return 100;
                break;
            default:
                // Other, not predicted cases
                return 5;
        }
    } else if (val.isString() || val.isDate() || val.isRegExp()) {
        switch (destTypeId){
            case QMetaType::Bool:
                // Conversion from to bool should be avoided if possible, it's probably not what we want
                return 100;
            case QMetaType::UInt:
            case QMetaType::ULong:
            case QMetaType::Int:
            case QMetaType::Long:
            case QMetaType::Short:
            case QMetaType::Double:
            case QMetaType::Float:
            case QMetaType::ULongLong:
            case QMetaType::LongLong:
            case QMetaType::UShort:
                // Conversion from to number should be avoided if possible, it's probably not what we want
                return 100;
            case QMetaType::QString:
                // Perfect case
                return 0;
            case QMetaType::QByteArray:
            case QMetaType::QDateTime:
            case QMetaType::QDate:
                // String to string should be slightly preferred
                return 5;
            default:
                return 5;
        }
    } else if (val.isBool() || val.isBoolean()) {
        switch (destTypeId){
            case QMetaType::Bool:
                // Perfect case
                return 0;
                break;
            case QMetaType::UInt:
            case QMetaType::ULong:
            case QMetaType::Int:
            case QMetaType::Long:
            case QMetaType::Short:
            case QMetaType::Double:
            case QMetaType::Float:
            case QMetaType::ULongLong:
            case QMetaType::LongLong:
            case QMetaType::UShort:
                // Using function with bool parameter should be preferred over converted bool to nimber
                return 5;
                break;
            case QMetaType::QString:
            case QMetaType::QByteArray:
            case QMetaType::QDateTime:
            case QMetaType::QDate:
                // Bool probably shouldn't be converted to string if there are better alternatives
                return 50;
                break;
            default:
                return 5;
        }
    }
    return 0;
}

bool ScriptEngineQtScript::castValueToVariant(const QScriptValue& val, QVariant& dest, int destTypeId) {

    // if we're not particularly interested in a specific type, try to detect if we're dealing with a registered type
    if (destTypeId == QMetaType::UnknownType) {
        QObject* obj = ScriptObjectQtProxy::unwrap(val);
        if (obj) {
            for (const QMetaObject* metaObject = obj->metaObject(); metaObject; metaObject = metaObject->superClass()) {
                QByteArray typeName = QByteArray(metaObject->className()) + "*";
                int typeId = QMetaType::type(typeName.constData());
                if (typeId != QMetaType::UnknownType) {
                    destTypeId = typeId;
                    break;
                }
            }
        }
    }

    if (destTypeId == qMetaTypeId<ScriptValue>()) {
        dest = QVariant::fromValue(ScriptValue(new ScriptValueQtWrapper(this, val)));
        return true;
    }

    // do we have a registered handler for this type?
    ScriptEngine::DemarshalFunction demarshalFunc = nullptr;
    {
        _customTypeProtect.lockForRead();
        CustomMarshalMap::const_iterator lookup = _customTypes.find(destTypeId);
        if (lookup != _customTypes.cend()) {
            demarshalFunc = lookup.value().demarshalFunc;
        }
        _customTypeProtect.unlock();
    }
    if (demarshalFunc) {
        dest = QVariant(destTypeId, static_cast<void*>(NULL));
        ScriptValue wrappedVal(new ScriptValueQtWrapper(this, val));
        bool success = demarshalFunc(wrappedVal, const_cast<void*>(dest.constData()));
        if(!success) dest = QVariant();
        return success;
    } else {
        switch (destTypeId) {
            case QMetaType::UnknownType:
                if (val.isUndefined()) {
                    dest = QVariant();
                    break;
                }
                if (val.isNull()) {
                    dest = QVariant::fromValue(nullptr);
                    break;
                }
                if (val.isBool()) {
                    dest = QVariant::fromValue(val.toBool());
                    break;
                }
                if (val.isString()) {
                    dest = QVariant::fromValue(val.toString());
                    break;
                }
                if (val.isNumber()) {
                    dest = QVariant::fromValue(val.toNumber());
                    break;
                }
                {
                    QObject* obj = ScriptObjectQtProxy::unwrap(val);
                    if (obj) {
                        dest = QVariant::fromValue(obj);
                        break;
                    }
                }
                {
                    QVariant var = ScriptVariantQtProxy::unwrap(val);
                    if (var.isValid()) {
                        dest = var;
                        break;
                    }
                }
                dest = val.toVariant();
                break;
            case QMetaType::Bool:
                dest = QVariant::fromValue(val.toBool());
                break;
            case QMetaType::QDateTime:
            case QMetaType::QDate:
                Q_ASSERT(val.isDate());
                dest = QVariant::fromValue(val.toDateTime());
                break;
            case QMetaType::UInt:
            case QMetaType::ULong:
                if ( val.isArray() || val.isObject() ){
                    return false;
                }
                dest = QVariant::fromValue(val.toUInt32());
                break;
            case QMetaType::Int:
            case QMetaType::Long:
            case QMetaType::Short:
                if ( val.isArray() || val.isObject() ){
                    return false;
                }
                dest = QVariant::fromValue(val.toInt32());
                break;
            case QMetaType::Double:
            case QMetaType::Float:
            case QMetaType::ULongLong:
            case QMetaType::LongLong:
                if ( val.isArray() || val.isObject() ){
                    return false;
                }
                dest = QVariant::fromValue(val.toNumber());
                break;
            case QMetaType::QString:
            case QMetaType::QByteArray:
                dest = QVariant::fromValue(val.toString());
                break;
            case QMetaType::UShort:
                if ( val.isArray() || val.isObject() ){
                    return false;
                }
                dest = QVariant::fromValue(val.toUInt16());
                break;
            case QMetaType::QObjectStar:
                dest = QVariant::fromValue(ScriptObjectQtProxy::unwrap(val));
                break;
            default:
                // check to see if this is a pointer to a QObject-derived object
                if (QMetaType::typeFlags(destTypeId) & (QMetaType::PointerToQObject | QMetaType::TrackingPointerToQObject)) {
                    /* Do we really want to permit regular passing of nullptr to native functions?
                    if (!val.isValid() || val.isUndefined() || val.isNull()) {
                        dest = QVariant::fromValue(nullptr);
                        break;
                    }*/
                    QObject* obj = ScriptObjectQtProxy::unwrap(val);
                    if (!obj) return false;
                    const QMetaObject* destMeta = QMetaType::metaObjectForType(destTypeId);
                    Q_ASSERT(destMeta);
                    obj = destMeta->cast(obj);
                    if (!obj) return false;
                    dest = QVariant::fromValue(obj);
                    break;
                }
                // check to see if we have a registered prototype
                {
                    QVariant var = ScriptVariantQtProxy::unwrap(val);
                    if (var.isValid()) {
                        dest = var;
                        break;
                    }
                }
                // last chance, just convert it to a variant
                dest = val.toVariant();
                break;
        }
    }

    return destTypeId == QMetaType::UnknownType || dest.userType() == destTypeId || dest.convert(destTypeId);
}

QString ScriptEngineQtScript::valueType(const QScriptValue& val) {
    if (val.isUndefined()) {
        return "undefined";
    }
    if (val.isNull()) {
        return "null";
    }
    if (val.isBool()) {
        return "boolean";
    }
    if (val.isString()) {
        return "string";
    }
    if (val.isNumber()) {
        return "number";
    }
    {
        QObject* obj = ScriptObjectQtProxy::unwrap(val);
        if (obj) {
            QString objectName = obj->objectName();
            if (!objectName.isEmpty()) return objectName;
            return obj->metaObject()->className();
        }
    }
    {
        QVariant var = ScriptVariantQtProxy::unwrap(val);
        if (var.isValid()) {
            return var.typeName();
        }
    }
    return val.toVariant().typeName();
}

QScriptValue ScriptEngineQtScript::castVariantToValue(const QVariant& val) {
    int valTypeId = val.userType();

    if (valTypeId == qMetaTypeId<ScriptValue>()) {
        // this is a wrapped ScriptValue, so just unwrap it and call it good
        ScriptValue innerVal = val.value<ScriptValue>();
        return ScriptValueQtWrapper::fullUnwrap(this, innerVal);
    }

    // do we have a registered handler for this type?
    ScriptEngine::MarshalFunction marshalFunc = nullptr;
    {
        _customTypeProtect.lockForRead();
        CustomMarshalMap::const_iterator lookup = _customTypes.find(valTypeId);
        if (lookup != _customTypes.cend()) {
            marshalFunc = lookup.value().marshalFunc;
        }
        _customTypeProtect.unlock();
    }
    if (marshalFunc) {
        ScriptValue wrappedVal = marshalFunc(this, val.constData());
        return ScriptValueQtWrapper::fullUnwrap(this, wrappedVal);
    }

    switch (valTypeId) {
        case QMetaType::UnknownType:
        case QMetaType::Void:
            return QScriptValue(this, QScriptValue::UndefinedValue);
        case QMetaType::Nullptr:
            return QScriptValue(this, QScriptValue::NullValue);
        case QMetaType::Bool:
            return QScriptValue(this, val.toBool());
        case QMetaType::Int:
        case QMetaType::Long:
        case QMetaType::Short:
            return QScriptValue(this, val.toInt());
        case QMetaType::UInt:
        case QMetaType::ULong:
        case QMetaType::UShort:
            return QScriptValue(this, val.toUInt());
        case QMetaType::Float:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Double:
            return QScriptValue(this, val.toFloat());
        case QMetaType::QString:
        case QMetaType::QByteArray:
            return QScriptValue(this, val.toString());
        case QMetaType::QVariant:
            return castVariantToValue(val.value<QVariant>());
        case QMetaType::QObjectStar: {
            QObject* obj = val.value<QObject*>();
            if (obj == nullptr) return QScriptValue(this, QScriptValue::NullValue);
            return ScriptObjectQtProxy::newQObject(this, obj);
        }
        case QMetaType::QDateTime:
            return static_cast<QScriptEngine*>(this)->newDate(val.value<QDateTime>());
        case QMetaType::QDate:
            return static_cast<QScriptEngine*>(this)->newDate(val.value<QDate>().startOfDay());
        default:
            // check to see if this is a pointer to a QObject-derived object
            if (QMetaType::typeFlags(valTypeId) & (QMetaType::PointerToQObject | QMetaType::TrackingPointerToQObject)) {
                QObject* obj = val.value<QObject*>();
                if (obj == nullptr) return QScriptValue(this, QScriptValue::NullValue);
                return ScriptObjectQtProxy::newQObject(this, obj);
            }
            // have we set a prototype'd variant?
            {
                _customTypeProtect.lockForRead();
                CustomPrototypeMap::const_iterator lookup = _customPrototypes.find(valTypeId);
                if (lookup != _customPrototypes.cend()) {
                    return ScriptVariantQtProxy::newVariant(this, val, lookup.value());
                }
                _customTypeProtect.unlock();
            }
            // just do a generic variant
            return QScriptEngine::newVariant(val);
    }
}
