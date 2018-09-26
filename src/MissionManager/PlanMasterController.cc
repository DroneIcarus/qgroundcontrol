/****************************************************************************
 *
 *   (c) 2009-2016 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "PlanMasterController.h"
#include "QGCApplication.h"
#include "QGCCorePlugin.h"
#include "MultiVehicleManager.h"
#include "SettingsManager.h"
#include "AppSettings.h"
#include "JsonHelper.h"
#include "MissionManager.h"
#include "KML.h"
#if defined(QGC_AIRMAP_ENABLED)
#include "AirspaceFlightPlanProvider.h"
#endif

#include <QDomDocument>
#include <QJsonDocument>
#include <QFileInfo>

#include <QJsonDocument>
#include <QFileInfo>

#include <QDateTime>
#include <QMessageBox>

#include <QFormLayout>
#include <QTableWidget>
#include <QInputDialog>
#include <QLabel>
#include <QSpinBox>
#include <QDialogButtonBox>

#include <QCalendarWidget>



QGC_LOGGING_CATEGORY(PlanMasterControllerLog, "PlanMasterControllerLog")

const int   PlanMasterController::kPlanFileVersion =            1;
const char* PlanMasterController::kPlanFileType =               "Plan";
const char* PlanMasterController::kJsonMissionObjectKey =       "mission";
const char* PlanMasterController::kJsonGeoFenceObjectKey =      "geoFence";
const char* PlanMasterController::kJsonRallyPointsObjectKey =   "rallyPoints";

PlanMasterController::PlanMasterController(QObject* parent)
    : QObject               (parent)
    , _multiVehicleMgr      (qgcApp()->toolbox()->multiVehicleManager())
    , _controllerVehicle    (new Vehicle(
        static_cast<MAV_AUTOPILOT>(qgcApp()->toolbox()->settingsManager()->appSettings()->offlineEditingFirmwareType()->rawValue().toInt()),
        static_cast<MAV_TYPE>(qgcApp()->toolbox()->settingsManager()->appSettings()->offlineEditingVehicleType()->rawValue().toInt()),
        qgcApp()->toolbox()->firmwarePluginManager()))
    , _managerVehicle       (_controllerVehicle)
    , _flyView              (true)
    , _offline              (true)
    , _missionController    (this)
    , _geoFenceController   (this)
    , _rallyPointController (this)
    , _loadGeoFence         (false)
    , _loadRallyPoints      (false)
    , _sendGeoFence         (false)
    , _sendRallyPoints      (false)
{
    connect(&_missionController,    &MissionController::dirtyChanged,       this, &PlanMasterController::dirtyChanged);
    connect(&_geoFenceController,   &GeoFenceController::dirtyChanged,      this, &PlanMasterController::dirtyChanged);
    connect(&_rallyPointController, &RallyPointController::dirtyChanged,    this, &PlanMasterController::dirtyChanged);

    connect(&_missionController,    &MissionController::containsItemsChanged,       this, &PlanMasterController::containsItemsChanged);
    connect(&_geoFenceController,   &GeoFenceController::containsItemsChanged,      this, &PlanMasterController::containsItemsChanged);
    connect(&_rallyPointController, &RallyPointController::containsItemsChanged,    this, &PlanMasterController::containsItemsChanged);

    connect(&_missionController,    &MissionController::syncInProgressChanged,      this, &PlanMasterController::syncInProgressChanged);
    connect(&_geoFenceController,   &GeoFenceController::syncInProgressChanged,     this, &PlanMasterController::syncInProgressChanged);
    connect(&_rallyPointController, &RallyPointController::syncInProgressChanged,   this, &PlanMasterController::syncInProgressChanged);
}

PlanMasterController::~PlanMasterController()
{

}

void PlanMasterController::start(bool flyView)
{
    _flyView = flyView;
    _missionController.start(_flyView);
    _geoFenceController.start(_flyView);
    _rallyPointController.start(_flyView);

    connect(_multiVehicleMgr, &MultiVehicleManager::activeVehicleChanged, this, &PlanMasterController::_activeVehicleChanged);
    _activeVehicleChanged(_multiVehicleMgr->activeVehicle());

#if defined(QGC_AIRMAP_ENABLED)
    //-- This assumes there is one single instance of PlanMasterController in edit mode.
    if(!flyView) {
        qgcApp()->toolbox()->airspaceManager()->flightPlan()->startFlightPlanning(this);
    }
#endif
}

void PlanMasterController::startStaticActiveVehicle(Vehicle* vehicle)
{
    _flyView = true;
    _missionController.start(_flyView);
    _geoFenceController.start(_flyView);
    _rallyPointController.start(_flyView);
    _activeVehicleChanged(vehicle);
}

void PlanMasterController::_activeVehicleChanged(Vehicle* activeVehicle)
{
    if (_managerVehicle == activeVehicle) {
        // We are already setup for this vehicle
        return;
    }

    qCDebug(PlanMasterControllerLog) << "_activeVehicleChanged" << activeVehicle;

    if (_managerVehicle) {
        // Disconnect old vehicle
        disconnect(_managerVehicle->missionManager(),       &MissionManager::newMissionItemsAvailable,  this, &PlanMasterController::_loadMissionComplete);
        disconnect(_managerVehicle->geoFenceManager(),      &GeoFenceManager::loadComplete,             this, &PlanMasterController::_loadGeoFenceComplete);
        disconnect(_managerVehicle->rallyPointManager(),    &RallyPointManager::loadComplete,           this, &PlanMasterController::_loadRallyPointsComplete);
        disconnect(_managerVehicle->missionManager(),       &MissionManager::sendComplete,              this, &PlanMasterController::_sendMissionComplete);
        disconnect(_managerVehicle->geoFenceManager(),      &GeoFenceManager::sendComplete,             this, &PlanMasterController::_sendGeoFenceComplete);
        disconnect(_managerVehicle->rallyPointManager(),    &RallyPointManager::sendComplete,           this, &PlanMasterController::_sendRallyPointsComplete);
    }

    bool newOffline = false;
    if (activeVehicle == nullptr) {
        // Since there is no longer an active vehicle we use the offline controller vehicle as the manager vehicle
        _managerVehicle = _controllerVehicle;
        newOffline = true;
    } else {
        newOffline = false;
        _managerVehicle = activeVehicle;

        // Update controllerVehicle to the currently connected vehicle
        AppSettings* appSettings = qgcApp()->toolbox()->settingsManager()->appSettings();
        appSettings->offlineEditingFirmwareType()->setRawValue(AppSettings::offlineEditingFirmwareTypeFromFirmwareType(_managerVehicle->firmwareType()));
        appSettings->offlineEditingVehicleType()->setRawValue(AppSettings::offlineEditingVehicleTypeFromVehicleType(_managerVehicle->vehicleType()));

        // We use these signals to sequence upload and download to the multiple controller/managers
        connect(_managerVehicle->missionManager(),      &MissionManager::newMissionItemsAvailable,  this, &PlanMasterController::_loadMissionComplete);
        connect(_managerVehicle->geoFenceManager(),     &GeoFenceManager::loadComplete,             this, &PlanMasterController::_loadGeoFenceComplete);
        connect(_managerVehicle->rallyPointManager(),   &RallyPointManager::loadComplete,           this, &PlanMasterController::_loadRallyPointsComplete);
        connect(_managerVehicle->missionManager(),      &MissionManager::sendComplete,              this, &PlanMasterController::_sendMissionComplete);
        connect(_managerVehicle->geoFenceManager(),     &GeoFenceManager::sendComplete,             this, &PlanMasterController::_sendGeoFenceComplete);
        connect(_managerVehicle->rallyPointManager(),   &RallyPointManager::sendComplete,           this, &PlanMasterController::_sendRallyPointsComplete);
    }

    _missionController.managerVehicleChanged(_managerVehicle);
    _geoFenceController.managerVehicleChanged(_managerVehicle);
    _rallyPointController.managerVehicleChanged(_managerVehicle);

    // Vehicle changed so we need to signal everything
    _offline = newOffline;
    emit containsItemsChanged(containsItems());
    emit syncInProgressChanged();
    emit dirtyChanged(dirty());
    emit offlineChanged(offline());

    if (!_flyView) {
        if (!offline()) {
            // We are in Plan view and we have a newly connected vehicle:
            //  - If there is no plan available in Plan view show the one from the vehicle
            //  - Otherwise leave the current plan alone
            if (!containsItems()) {
                qCDebug(PlanMasterControllerLog) << "_activeVehicleChanged: Plan view is empty so loading from manager";
                _showPlanFromManagerVehicle();
            }
        }
    } else {
        if (offline()) {
            // No more active vehicle, clear mission
            qCDebug(PlanMasterControllerLog) << "_activeVehicleChanged: Fly view is offline clearing plan";
            removeAll();
        } else {
            // Fly view has changed to a new active vehicle, update to show correct mission
            qCDebug(PlanMasterControllerLog) << "_activeVehicleChanged: Fly view is online so loading from manager";
            _showPlanFromManagerVehicle();
        }
    }
}

void PlanMasterController::loadFromVehicle(void)
{
    if (_managerVehicle->highLatencyLink()) {
        qgcApp()->showMessage(tr("Download not supported on high latency links."));
        return;
    }

    if (offline()) {
        qCWarning(PlanMasterControllerLog) << "PlanMasterController::loadFromVehicle called while offline";
    } else if (_flyView) {
        qCWarning(PlanMasterControllerLog) << "PlanMasterController::loadFromVehicle called from Fly view";
    } else if (syncInProgress()) {
        qCWarning(PlanMasterControllerLog) << "PlanMasterController::loadFromVehicle called while syncInProgress";
    } else {
        _loadGeoFence = true;
        qCDebug(PlanMasterControllerLog) << "PlanMasterController::loadFromVehicle calling _missionController.loadFromVehicle";
        _missionController.loadFromVehicle();
        setDirty(false);
    }
}


void PlanMasterController::_loadMissionComplete(void)
{
    if (!_flyView && _loadGeoFence) {
        _loadGeoFence = false;
        _loadRallyPoints = true;
        if (_geoFenceController.supported()) {
            qCDebug(PlanMasterControllerLog) << "PlanMasterController::_loadMissionComplete calling _geoFenceController.loadFromVehicle";
            _geoFenceController.loadFromVehicle();
        } else {
            qCDebug(PlanMasterControllerLog) << "PlanMasterController::_loadMissionComplete GeoFence not supported skipping";
            _geoFenceController.removeAll();
            _loadGeoFenceComplete();
        }
        setDirty(false);
    }
}

void PlanMasterController::_loadGeoFenceComplete(void)
{
    if (!_flyView && _loadRallyPoints) {
        _loadRallyPoints = false;
        if (_rallyPointController.supported()) {
            qCDebug(PlanMasterControllerLog) << "PlanMasterController::_loadGeoFenceComplete calling _rallyPointController.loadFromVehicle";
            _rallyPointController.loadFromVehicle();
        } else {
            qCDebug(PlanMasterControllerLog) << "PlanMasterController::_loadMissionComplete Rally Points not supported skipping";
            _rallyPointController.removeAll();
            _loadRallyPointsComplete();
        }
        setDirty(false);
    }
}

void PlanMasterController::_loadRallyPointsComplete(void)
{
    qCDebug(PlanMasterControllerLog) << "PlanMasterController::_loadRallyPointsComplete";
}

void PlanMasterController::_sendMissionComplete(void)
{
    if (_sendGeoFence) {
        _sendGeoFence = false;
        _sendRallyPoints = true;
        if (_geoFenceController.supported()) {
            qCDebug(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle start GeoFence sendToVehicle";
            _geoFenceController.sendToVehicle();
        } else {
            qCDebug(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle GeoFence not supported skipping";
            _sendGeoFenceComplete();
        }
        setDirty(false);
    }
}

void PlanMasterController::_sendGeoFenceComplete(void)
{
    if (_sendRallyPoints) {
        _sendRallyPoints = false;
        if (_rallyPointController.supported()) {
            qCDebug(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle start rally sendToVehicle";
            _rallyPointController.sendToVehicle();
        } else {
            qCDebug(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle Rally Points not support skipping";
            _sendRallyPointsComplete();
        }
    }
}

void PlanMasterController::_sendRallyPointsComplete(void)
{
    qCDebug(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle Rally Point send complete";
}

void PlanMasterController::sendToVehicle(void)
{
    if (_managerVehicle->highLatencyLink()) {
        qgcApp()->showMessage(tr("Upload not supported on high latency links."));
        return;
    }

    if (offline()) {
        qCWarning(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle called while offline";
    } else if (syncInProgress()) {
        qCWarning(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle called while syncInProgress";
    } else {
        qCDebug(PlanMasterControllerLog) << "PlanMasterController::sendToVehicle start mission sendToVehicle";
        _sendGeoFence = true;
        _missionController.sendToVehicle();
        setDirty(false);
    }
}

void PlanMasterController::loadFromFile(const QString& filename)
{
    QString errorString;
    QString errorMessage = tr("Error loading Plan file (%1). %2").arg(filename).arg("%1");

    if (filename.isEmpty()) {
        return;
    }

    QFileInfo fileInfo(filename);
    QFile file(filename);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errorString = file.errorString() + QStringLiteral(" ") + filename;
        qgcApp()->showMessage(errorMessage.arg(errorString));
        return;
    }

    bool success = false;
    if(fileInfo.suffix() == AppSettings::planFileExtension) {
        QJsonDocument   jsonDoc;
        QByteArray      bytes = file.readAll();

        if (!JsonHelper::isJsonFile(bytes, jsonDoc, errorString)) {
            qgcApp()->showMessage(errorMessage.arg(errorString));
            return;
        }

        QJsonObject json = jsonDoc.object();
        //-- Allow plugins to pre process the load
        qgcApp()->toolbox()->corePlugin()->preLoadFromJson(this, json);

        int version;
        if (!JsonHelper::validateQGCJsonFile(json, kPlanFileType, kPlanFileVersion, kPlanFileVersion, version, errorString)) {
            qgcApp()->showMessage(errorMessage.arg(errorString));
            return;
        }

        QList<JsonHelper::KeyValidateInfo> rgKeyInfo = {
            { kJsonMissionObjectKey,        QJsonValue::Object, true },
            { kJsonGeoFenceObjectKey,       QJsonValue::Object, true },
            { kJsonRallyPointsObjectKey,    QJsonValue::Object, true },
        };
        if (!JsonHelper::validateKeys(json, rgKeyInfo, errorString)) {
            qgcApp()->showMessage(errorMessage.arg(errorString));
            return;
        }

        if (!_missionController.load(json[kJsonMissionObjectKey].toObject(), errorString) ||
                !_geoFenceController.load(json[kJsonGeoFenceObjectKey].toObject(), errorString) ||
                !_rallyPointController.load(json[kJsonRallyPointsObjectKey].toObject(), errorString)) {
            qgcApp()->showMessage(errorMessage.arg(errorString));
        } else {
            //-- Allow plugins to post process the load
            qgcApp()->toolbox()->corePlugin()->postLoadFromJson(this, json);
            success = true;
        }
    } else if (fileInfo.suffix() == AppSettings::missionFileExtension) {
        if (!_missionController.loadJsonFile(file, errorString)) {
            qgcApp()->showMessage(errorMessage.arg(errorString));
        } else {
            success = true;
        }
    } else if (fileInfo.suffix() == AppSettings::waypointsFileExtension || fileInfo.suffix() == QStringLiteral("txt")) {
        if (!_missionController.loadTextFile(file, errorString)) {
            qgcApp()->showMessage(errorMessage.arg(errorString));
        } else {
            success = true;
        }
    } else {
        //-- TODO: What then?
    }

    if(success){
        _currentPlanFile.sprintf("%s/%s.%s", fileInfo.path().toLocal8Bit().data(), fileInfo.completeBaseName().toLocal8Bit().data(), AppSettings::planFileExtension);
    } else {
        _currentPlanFile.clear();
    }
    emit currentPlanFileChanged();

    if (!offline()) {
        setDirty(true);
    }
}

QJsonDocument PlanMasterController::saveToJson()
{
    QJsonObject planJson;
    qgcApp()->toolbox()->corePlugin()->preSaveToJson(this, planJson);
    QJsonObject missionJson;
    QJsonObject fenceJson;
    QJsonObject rallyJson;
    JsonHelper::saveQGCJsonFileHeader(planJson, kPlanFileType, kPlanFileVersion);
    //-- Allow plugin to preemptly add its own keys to mission
    qgcApp()->toolbox()->corePlugin()->preSaveToMissionJson(this, missionJson);
    _missionController.save(missionJson);
    //-- Allow plugin to add its own keys to mission
    qgcApp()->toolbox()->corePlugin()->postSaveToMissionJson(this, missionJson);
    _geoFenceController.save(fenceJson);
    _rallyPointController.save(rallyJson);
    planJson[kJsonMissionObjectKey] = missionJson;
    planJson[kJsonGeoFenceObjectKey] = fenceJson;
    planJson[kJsonRallyPointsObjectKey] = rallyJson;
    qgcApp()->toolbox()->corePlugin()->postSaveToJson(this, planJson);
    return QJsonDocument(planJson);
}

void
PlanMasterController::saveToCurrent()
{
    if(!_currentPlanFile.isEmpty()) {
        saveToFile(_currentPlanFile);
    }
}

void PlanMasterController::saveToFile(const QString& filename)
{
    if (filename.isEmpty()) {
        return;
    }

    QString planFilename = filename;
    if (!QFileInfo(filename).fileName().contains(".")) {
        planFilename += QString(".%1").arg(fileExtension());
    }

    QFile file(planFilename);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qgcApp()->showMessage(tr("Plan save error %1 : %2").arg(filename).arg(file.errorString()));
        _currentPlanFile.clear();
        emit currentPlanFileChanged();
    } else {
        QJsonDocument saveDoc = saveToJson();
        file.write(saveDoc.toJson());
        if(_currentPlanFile != planFilename) {
            _currentPlanFile = planFilename;
            emit currentPlanFileChanged();
        }
    }

    // Only clear dirty bit if we are offline
    if (offline()) {
        setDirty(false);
    }
}

void PlanMasterController::saveToKml(const QString& filename)
{
    if (filename.isEmpty()) {
        return;
    }

    QString kmlFilename = filename;
    if (!QFileInfo(filename).fileName().contains(".")) {
        kmlFilename += QString(".%1").arg(kmlFileExtension());
    }

    QFile file(kmlFilename);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qgcApp()->showMessage(tr("KML save error %1 : %2").arg(filename).arg(file.errorString()));
    } else {
        QDomDocument domDocument;
        _missionController.convertToKMLDocument(domDocument);
        QTextStream stream(&file);
        stream << domDocument.toString();
        file.close();
    }
}

void PlanMasterController::removeAll(void)
{
    _missionController.removeAll();
    _geoFenceController.removeAll();
    _rallyPointController.removeAll();
    if (_offline) {
        _missionController.setDirty(false);
        _geoFenceController.setDirty(false);
        _rallyPointController.setDirty(false);
        _currentPlanFile.clear();
        emit currentPlanFileChanged();
    }
}

void PlanMasterController::removeAllFromVehicle(void)
{
    if (!offline()) {
        _missionController.removeAllFromVehicle();
        if (_geoFenceController.supported()) {
            _geoFenceController.removeAllFromVehicle();
        }
        if (_rallyPointController.supported()) {
            _rallyPointController.removeAllFromVehicle();
        }
        setDirty(false);
    } else {
        qWarning() << "PlanMasterController::removeAllFromVehicle called while offline";
    }
}

bool PlanMasterController::containsItems(void) const
{
    return _missionController.containsItems() || _geoFenceController.containsItems() || _rallyPointController.containsItems();
}

bool PlanMasterController::dirty(void) const
{
    return _missionController.dirty() || _geoFenceController.dirty() || _rallyPointController.dirty();
}

void PlanMasterController::setDirty(bool dirty)
{
    _missionController.setDirty(dirty);
    _geoFenceController.setDirty(dirty);
    _rallyPointController.setDirty(dirty);
}

QString PlanMasterController::fileExtension(void) const
{
    return AppSettings::planFileExtension;
}

QString PlanMasterController::kmlFileExtension(void) const
{
    return AppSettings::kmlFileExtension;
}

QStringList PlanMasterController::loadNameFilters(void) const
{
    QStringList filters;

    filters << tr("Supported types (*.%1 *.%2 *.%3 *.%4)").arg(AppSettings::planFileExtension).arg(AppSettings::missionFileExtension).arg(AppSettings::waypointsFileExtension).arg("txt") <<
               tr("All Files (*.*)");
    return filters;
}


QStringList PlanMasterController::saveNameFilters(void) const
{
    QStringList filters;

    filters << tr("Plan Files (*.%1)").arg(fileExtension()) << tr("All Files (*.*)");
    return filters;
}

QStringList PlanMasterController::fileKmlFilters(void) const
{
    QStringList filters;

    filters << tr("KML Files (*.%1)").arg(kmlFileExtension()) << tr("All Files (*.*)");
    return filters;
}

void PlanMasterController::sendPlanToVehicle(Vehicle* vehicle, const QString& filename)
{
    // Use a transient PlanMasterController to accomplish this
    PlanMasterController* controller = new PlanMasterController();
    controller->startStaticActiveVehicle(vehicle);
    controller->loadFromFile(filename);
    controller->sendToVehicle();
    delete controller;
}

void PlanMasterController::_showPlanFromManagerVehicle(void)
{
    if (!_managerVehicle->initialPlanRequestComplete() && !syncInProgress()) {
        // Something went wrong with initial load. All controllers are idle, so just force it off
        _managerVehicle->forceInitialPlanRequestComplete();
    }

    // The crazy if structure is to handle the load propagating by itself through the system
    if (!_missionController.showPlanFromManagerVehicle()) {
        if (!_geoFenceController.showPlanFromManagerVehicle()) {
            _rallyPointController.showPlanFromManagerVehicle();
        }
    }
}

bool PlanMasterController::syncInProgress(void) const
{
    return _missionController.syncInProgress() ||
            _geoFenceController.syncInProgress() ||
            _rallyPointController.syncInProgress();
}

QJsonDocument readJson(QString path)
{
    QString val;
    QFile file;
    file.setFileName(path);
    file.open(QIODevice::ReadOnly | QIODevice::Text);
    val = file.readAll();
    file.close();
    QJsonDocument d = QJsonDocument::fromJson(val.toUtf8());
    QJsonObject sett2 = d.object();
    return d;
}

void writeJson(QString path, QJsonDocument jsonDoc)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(jsonDoc.toJson());
        file.close();
    } else {
        qDebug() << "File wasn't openend";
    }
}

QJsonObject showMissionSettingsDialog(QJsonObject missionSettings) {
    bool accepted = false;
    QDialog dialog(0);
    // Use a layout allowing to have a label next to each field
    QFormLayout form(&dialog);

    // Add text above the fields
    form.addRow(new QLabel("Please enter the settings for the mission."));

    QList<QLineEdit *> fields;

    // Add the Speed Drone Fields
    QSpinBox *droneSpeedSpinBox = new QSpinBox(&dialog);
    droneSpeedSpinBox->setRange(10,100);
    droneSpeedSpinBox->setValue(missionSettings["droneMaxSpeed"].toInt());
    QString droneSpeedLabel = QString("Drone maximum speed (km/h) : ");
    form.addRow(droneSpeedLabel, droneSpeedSpinBox);

    // Add Autonomy fields
    QSpinBox *droneAutonomySpinBox = new QSpinBox(&dialog);
    droneAutonomySpinBox->setRange(10,50);
    droneAutonomySpinBox->setValue(missionSettings["droneAutonomy"].toInt());
    QString droneAutonomyLabel = QString("Drone autonomy (min) : ");
    form.addRow(droneAutonomyLabel, droneAutonomySpinBox);

    // Add charging fields
    QSpinBox *droneChargingSpinBox = new QSpinBox(&dialog);
    droneChargingSpinBox->setRange(30,100);
    droneChargingSpinBox->setValue(missionSettings["droneChargingTime"].toInt());
    QString droneChargingTimeLabel = QString("Drone charging time (min.) : ");
    form.addRow(droneChargingTimeLabel, droneChargingSpinBox);

    // Add charging efficiency fields
    QSpinBox *droneChargingEfficiencySpinBox = new QSpinBox(&dialog);
    droneChargingEfficiencySpinBox->setRange(0,100);
    droneChargingEfficiencySpinBox->setValue(missionSettings["droneChargeEfficiency"].toInt());
    QString droneChargingEfficiencyLabel = QString("Drone charging efficiency (%) : ");
    form.addRow(droneChargingEfficiencyLabel, droneChargingEfficiencySpinBox);

    // Add buttons (Cancel/Ok) at the bottom of the dialog
    QDialogButtonBox buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                               Qt::Horizontal, &dialog);
    form.addRow(&buttonBox);
    QObject::connect(&buttonBox, SIGNAL(accepted()), &dialog, SLOT(accept()));
    QObject::connect(&buttonBox, SIGNAL(rejected()), &dialog, SLOT(reject()));

    // Show the dialog as modal

    int dialogCode = dialog.exec();
    if (dialogCode == QDialog::Accepted) {
        // If the user didn't dismiss the dialog, do something with the fields

        missionSettings["droneAutonomy"] = droneAutonomySpinBox->value();
        missionSettings["droneMaxSpeed"] = droneSpeedSpinBox->value();
        missionSettings["droneChargingTime"] = droneChargingSpinBox->value();
        missionSettings["droneChargeEfficiency"] = droneChargingEfficiencySpinBox->value();
        missionSettings["accept"] = true;
        accepted = true;
    } else if (dialogCode == QDialog::Rejected)
    {
        missionSettings["accept"] = false;
    }
    return missionSettings;
}

void PlanMasterController::replyFinished(QNetworkReply* reply)
{
    QMessageBox msgBox;

    QDateTime now = QDateTime::currentDateTime();
    QString fileGeneratedPlan = tr(qgcApp()->toolbox()->settingsManager()->appSettings()->missionSavePath().toUtf8().constData())
                        + QString("/") + now.toString("yyyy-MM-dd_HH-mm") + QString("_generated")
                        + QString(".") + tr( AppSettings::planFileExtension);




    if (reply->error() == QNetworkReply::NoError) {

        QByteArray temp = reply->readAll();
        QJsonParseError error;
        QJsonDocument  jsonDoc = QJsonDocument::fromJson(temp, &error);

        if(error.error != 0) {
            msgBox.critical(0,"Error", error.errorString());
            return;
        }
        if (!jsonDoc.isObject()) {
            msgBox.critical(0,"Error","Bad server response !");
            return;
        }

        QJsonObject responseJsonObject = jsonDoc.object();

        if(!responseJsonObject["error"].isNull()) {
            msgBox.critical(0,"Error",responseJsonObject["error"].toString());
            qDebug() <<responseJsonObject["error"];
            return;
        }

        QJsonDocument plan(responseJsonObject["plan"].toObject());
        QFile jsonFile(fileGeneratedPlan);
        jsonFile.open(QFile::WriteOnly);
        jsonFile.write(plan.toJson());
        jsonFile.close();
        qDebug() << "New generated file written : " + fileGeneratedPlan;

        PlanMasterController::loadFromFile(fileGeneratedPlan);

        msgBox.setIcon(QMessageBox::Information);
        msgBox.setText("Success while executing external scripts!");

    } else {
         msgBox.critical(0,"Error","Server error !");
    }
}


void PlanMasterController::startCustomCode()
{

    QJsonObject planJson;
    QJsonObject missionJson;
    QJsonObject fenceJson;
    QJsonObject rallyJson;
    QString missionPlanner;

#if defined (__macos__)
    missionPlanner = qgcApp()->applicationDirPath().toUtf8() + "/../../../../../MissionOptimizer";
#else
    missionPlanner = qgcApp()->applicationDirPath().toUtf8() + "/../../MissionOptimizer";
#endif

    // Read mission settings from JSON file and transfer into JSON obj
    QJsonDocument missionSettings = readJson(missionPlanner + "/missionSettings.json");
    QJsonObject missionSettingsObj(missionSettings.object());

    QJsonObject newMissionSettings = showMissionSettingsDialog(missionSettingsObj);

    if (!newMissionSettings["accept"].toBool()) {
        return;
    }

    // Transfer new settings JSON obj into JSON doc and overwrite the file
    QJsonDocument missionSettingsDoc(newMissionSettings);
    writeJson(missionPlanner + "/missionSettings.json", missionSettingsDoc);

    // Transfer settings JSON doc into compacted string to pass in args
    QString missionSettingsStr(missionSettingsDoc.toJson(QJsonDocument::Compact));

    JsonHelper::saveQGCJsonFileHeader(planJson, kPlanFileType, kPlanFileVersion);
    _missionController.save(missionJson);
    _geoFenceController.save(fenceJson);
    _rallyPointController.save(rallyJson);
    planJson[kJsonMissionObjectKey] = missionJson;
    planJson[kJsonGeoFenceObjectKey] = fenceJson;
    planJson[kJsonRallyPointsObjectKey] = rallyJson;

    QJsonDocument doc(planJson);
    QString strJson(doc.toJson(QJsonDocument::Compact));

    qDebug() << strJson;

    QNetworkAccessManager *restclient; //in class
    QNetworkRequest request;
    restclient = new QNetworkAccessManager(this); //constructor

    connect(restclient, SIGNAL(finished(QNetworkReply*)),this, SLOT(replyFinished(QNetworkReply*)));

    QUrl myurl;
    QUrlQuery querystr;
    querystr.addQueryItem("api_key","Key");
    querystr.addQueryItem("qgcPlan",strJson);
    querystr.addQueryItem("missionSettings",missionSettingsStr);
    myurl.setScheme("http");
    myurl.setHost("icarus.hopto.org");
    myurl.setPath("/drone/mission");
    myurl.setQuery(querystr);
    myurl.setPort(8080);
    request.setUrl(myurl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    qDebug() << "Calling server";
    qDebug() << myurl;
    restclient->get(request);



/*
    // Filenames in the form of current date and time
    QDateTime now = QDateTime::currentDateTime();
    QString fileOriginalPlan = tr(qgcApp()->toolbox()->settingsManager()->appSettings()->missionSavePath().toUtf8().constData())
                        + QString("/") + now.toString("yyyy-MM-dd_HH-mm")
                        + QString(".") + tr( AppSettings::planFileExtension);

    QString fileGeneratedPlan = tr(qgcApp()->toolbox()->settingsManager()->appSettings()->missionSavePath().toUtf8().constData())
                        + QString("/") + now.toString("yyyy-MM-dd_HH-mm") + QString("_generated")
                        + QString(".") + tr( AppSettings::planFileExtension);
   PlanMasterController::saveToFile(fileOriginalPlan);

    // Start a process and the console output will be at the parent environment
    QProcess *scriptsProc = new QProcess(parent());
    scriptsProc->setProcessChannelMode(QProcess::ForwardedOutputChannel);

#if defined (__macos__)
    missionPlanner = qgcApp()->applicationDirPath().toUtf8() + "/../../../../../MissionOptimizer";
    python = "/usr/local/bin/python3";
#else
    missionPlanner = qgcApp()->applicationDirPath().toUtf8() + "/../../MissionOptimizer";
    python = "/usr/bin/python3";
#endif

    scriptsProc->setWorkingDirectory(missionPlanner);

    // Read mission settings from JSON file and transfer into JSON obj
    QJsonDocument missionSettings = readJson(missionPlanner + "/missionSettings.json");
    QJsonObject missionSettingsObj(missionSettings.object());

    // Popup setting dialog
    QJsonObject newMissionSettings = showMissionSettingsDialog(missionSettingsObj);

    if (!newMissionSettings["accept"].toBool()) {
        return;
    }

    // Transfer new settings JSON obj into JSON doc and overwrite the file
    QJsonDocument missionSettingsDoc(newMissionSettings);
    writeJson(missionPlanner + "/missionSettings.json", missionSettingsDoc);

    // Transfer settings JSON doc into compacted string to pass in args
    QString missionSettingsStr(missionSettingsDoc.toJson(QJsonDocument::Compact));

    // Start MissionPlanner script with waypoints as args
    QStringList params = QStringList() << missionPlanner + "/main.py" << fileOriginalPlan << fileGeneratedPlan << missionSettingsStr;
    scriptsProc->start(python, params);

    if (!scriptsProc->waitForStarted(5000))
        errorString = "The script didn't start under the reserved time.";

    if (!scriptsProc->waitForFinished(30000))
        errorString = "The script didn't finish under the reserved time.";
    else
        errorString = scriptsProc->readAllStandardError();
    scriptsProc->close();

    if(scriptsProc->exitCode() == QProcess::NormalExit)
    {
        PlanMasterController::loadFromFile(fileGeneratedPlan);
        msgBox.setIcon(QMessageBox::Information);
        msgBox.setText("Success while executing external scripts!");
        //msgBox.exec();
    }
    else
    {
        if (errorString.isEmpty())
            msgBox.critical(0,"Error","Unhandled error.");
        else
            msgBox.critical(0,"Error",errorString);
    }

    delete scriptsProc;
    */
}
