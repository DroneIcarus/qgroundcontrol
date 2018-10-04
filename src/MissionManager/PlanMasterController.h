/****************************************************************************
 *
 *   (c) 2009-2016 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QObject>

#include "MissionController.h"
#include "GeoFenceController.h"
#include "RallyPointController.h"
#include "Vehicle.h"
#include "MultiVehicleManager.h"
#include "QGCLoggingCategory.h"

Q_DECLARE_LOGGING_CATEGORY(PlanMasterControllerLog)

/// Master controller for mission, fence, rally
class PlanMasterController : public QObject
{
    Q_OBJECT
    
public:
    PlanMasterController(QObject* parent = NULL);
    ~PlanMasterController();
    
    Q_PROPERTY(MissionController*       missionController       READ missionController      CONSTANT)
    Q_PROPERTY(GeoFenceController*      geoFenceController      READ geoFenceController     CONSTANT)
    Q_PROPERTY(RallyPointController*    rallyPointController    READ rallyPointController   CONSTANT)

    Q_PROPERTY(Vehicle*     controllerVehicle   MEMBER _controllerVehicle               CONSTANT)
    Q_PROPERTY(bool         offline             READ offline                            NOTIFY offlineChanged)          ///< true: controller is not connected to an active vehicle
    Q_PROPERTY(bool         containsItems       READ containsItems                      NOTIFY containsItemsChanged)    ///< true: Elemement is non-empty
    Q_PROPERTY(bool         syncInProgress      READ syncInProgress                     NOTIFY syncInProgressChanged)   ///< true: Information is currently being saved/sent, false: no active save/send in progress
    Q_PROPERTY(bool         dirty               READ dirty              WRITE setDirty  NOTIFY dirtyChanged)            ///< true: Unsaved/sent changes are present, false: no changes since last save/send
    Q_PROPERTY(QString      fileExtension       READ fileExtension                      CONSTANT)                       ///< File extension for missions
    Q_PROPERTY(QString      kmlFileExtension    READ kmlFileExtension                   CONSTANT)
    Q_PROPERTY(QString      currentPlanFile     READ currentPlanFile                    NOTIFY currentPlanFileChanged)
    ///< kml file extension for missions
    Q_PROPERTY(QStringList  loadNameFilters     READ loadNameFilters                    CONSTANT)                       ///< File filter list loading plan files
    Q_PROPERTY(QStringList  saveNameFilters     READ saveNameFilters                    CONSTANT)                       ///< File filter list saving plan files
    Q_PROPERTY(QStringList  fileKmlFilters      READ fileKmlFilters                     CONSTANT)                       ///< File filter list for load/save KML files

    /// Should be called immediately upon Component.onCompleted.
    Q_INVOKABLE void start(bool flyView);

    /// Starts the controller using a single static active vehicle. Will not track global active vehicle changes.
    Q_INVOKABLE void startStaticActiveVehicle(Vehicle* vehicle);

    /// Determines if the plan has all data needed to be saved or sent to the vehicle. Currently the only case where this
    /// would return false is when it is still waiting on terrain data to determine correct altitudes.
    Q_INVOKABLE bool readyForSaveSend(void) const { return _missionController.readyForSaveSend(); }

    /// Sends a plan to the specified file
    ///     @param[in] vehicle Vehicle we are sending a plan to
    ///     @param[in] filename Plan file to load
    static void sendPlanToVehicle(Vehicle* vehicle, const QString& filename);

    Q_INVOKABLE void loadFromVehicle(void);
    Q_INVOKABLE void sendToVehicle(void);
    Q_INVOKABLE void loadFromFile(const QString& filename);
    Q_INVOKABLE void saveToCurrent();
    Q_INVOKABLE void saveToFile(const QString& filename);
    Q_INVOKABLE void saveToKml(const QString& filename);
    Q_INVOKABLE void removeAll(void);                       ///< Removes all from controller only, synce required to remove from vehicle
    Q_INVOKABLE void removeAllFromVehicle(void);            ///< Removes all from vehicle and controller
    Q_INVOKABLE void startCustomCode();

    MissionController*      missionController(void)     { return &_missionController; }
    GeoFenceController*     geoFenceController(void)    { return &_geoFenceController; }
    RallyPointController*   rallyPointController(void)  { return &_rallyPointController; }

    bool        offline         (void) const { return _offline; }
    bool        containsItems   (void) const;
    bool        syncInProgress  (void) const;
    bool        dirty           (void) const;
    void        setDirty        (bool dirty);
    QString     fileExtension   (void) const;
    QString     kmlFileExtension(void) const;
    QString     currentPlanFile (void) const { return _currentPlanFile; }
    QStringList loadNameFilters (void) const;
    QStringList saveNameFilters (void) const;
    QStringList fileKmlFilters  (void) const;

    QJsonObject showMissionSettingsDialog(QJsonObject missionSettings);
    QJsonDocument readJson(QString path);
    void writeJson(QString path, QJsonDocument jsonDoc);

    QJsonDocument saveToJson    ();

    Vehicle* controllerVehicle(void) { return _controllerVehicle; }
    Vehicle* managerVehicle(void) { return _managerVehicle; }

    static const int    kPlanFileVersion;
    static const char*  kPlanFileType;
    static const char*  kJsonMissionObjectKey;
    static const char*  kJsonGeoFenceObjectKey;
    static const char*  kJsonRallyPointsObjectKey;

signals:
    void containsItemsChanged   (bool containsItems);
    void syncInProgressChanged  (void);
    void dirtyChanged           (bool dirty);
    void offlineChanged  		(bool offlineEditing);
    void currentPlanFileChanged ();

private slots:
    void _activeVehicleChanged(Vehicle* activeVehicle);
    void _loadMissionComplete(void);
    void _loadGeoFenceComplete(void);
    void _loadRallyPointsComplete(void);
    void _sendMissionComplete(void);
    void _sendGeoFenceComplete(void);
    void _sendRallyPointsComplete(void);
    void replyFinished(QNetworkReply* reply);

private:
    void _showPlanFromManagerVehicle(void);

    MultiVehicleManager*    _multiVehicleMgr;
    Vehicle*                _controllerVehicle; ///< Offline controller vehicle
    Vehicle*                _managerVehicle;    ///< Either active vehicle or _controllerVehicle if none
    bool                    _flyView;
    bool                    _offline;
    MissionController       _missionController;
    GeoFenceController      _geoFenceController;
    RallyPointController    _rallyPointController;
    bool                    _loadGeoFence;
    bool                    _loadRallyPoints;
    bool                    _sendGeoFence;
    bool                    _sendRallyPoints;
    QString                 _currentPlanFile;

};
