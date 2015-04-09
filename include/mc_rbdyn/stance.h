#ifndef _H_MCRBDYNSTANCE_H_
#define _H_MCRBDYNSTANCE_H_

#include <mc_rbdyn/contact.h>
#include <mc_rbdyn/robot.h>

namespace mc_rbdyn
{

static unsigned int nrConeGen = 4;
static double defaultFriction = 0.7;
static unsigned int nrBilatPoints = 4;

struct Stance
{
public:
  Stance(const std::vector< std::vector<double> > & q, const std::vector<Contact> & geomContacts, const std::vector<Contact> stabContacts);

  const std::vector<Contact> & contacts();

  void updateContact(const Contact & oldContact, const Contact & newContact);

  Eigen::Vector3d com(const Robot & robot);

  std::vector<std::string> robotSurfacesInContact();
public:
  std::vector< std::vector<double> > q;
  std::vector< Contact > geomContacts;
  std::vector< Contact > stabContacts;
};

typedef std::pair<std::vector<Contact>, std::vector<Contact> > contact_vector_pair_t;
typedef std::pair< contact_vector_pair_t, contact_vector_pair_t > apply_return_t;

struct StanceAction
{
  virtual apply_return_t apply(const Stance & stance) = 0;

  virtual void update(const Stance & stance) = 0;

  virtual std::string toStr() = 0;
};

struct IdentityContactAction : public StanceAction
{
public:
  virtual apply_return_t apply(const Stance & stance);

  virtual void update(const Stance & stance);

  virtual std::string toStr();
};

struct AddContactAction : public StanceAction
{
public:
  AddContactAction(const Contact & contact);

  virtual apply_return_t apply(const Stance & stance);

  virtual void update(const Stance & stance);

  virtual std::string toStr();
public:
  Contact contact;
};

struct RemoveContactAction : public StanceAction
{
public:
  RemoveContactAction(const Contact & contact);

  virtual apply_return_t apply(const Stance & stance);

  virtual void update(const Stance & stance);

  virtual std::string toStr();
public:
  Contact contact;
};

/*FIXME Need a python util to convert stance files generated by python into something readable by the C++ counterpart */

void loadStances(const std::string & filename, std::vector<Stance> & stances, std::vector< std::shared_ptr<StanceAction> > & actions);

void saveStances(const std::string & filename, std::vector<Stance> & stances, std::vector< std::shared_ptr<StanceAction> > & actions);

void saveStancesJSON(const std::string & filename, std::vector<Stance> & stances, std::vector< std::shared_ptr<StanceAction> > & actions);

}

#endif
